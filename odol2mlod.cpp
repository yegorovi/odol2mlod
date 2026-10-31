// odol2mlod.cpp - конвертер p3d из бинарного формата ODOL (DayZ/Arma) в редактируемый MLOD
// Поддерживает ODOL v28..v73 (проверено на v53 Arma 3 и v56 DayZ 1.29) и пишет MLOD v257 (P3DM LOD 28/256).
// Формат разобран по публичным описаниям P3D и открытым конвертерам (DeODOL55UNI, odol2mlod Mikero).
// Сборка: g++ -O2 -std=c++17 -o odol2mlod.exe odol2mlod.cpp
// Использование: odol2mlod <input.p3d> [output.p3d] [-v]  (output по умолчанию <input>_mlod.p3d, -v - трассировка чтения)

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <stdexcept>

// ---- трассировка ----

static bool g_trace = false;

// печать строки трассировки с текущей позицией чтения
// склеивает четыре сырых байта в uint32
static uint32_t packU32(const uint8_t* b)
{
	return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8)
		| (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

static void traceMsg(const char* msg)
{
	if (g_trace) {
		std::printf("[trace] %s\n", msg);
	}
}

// ---- читалка (весь файл в памяти, little-endian) ----

class Reader
{
public:
	std::vector<uint8_t> data;
	size_t pos = 0;
	int version = 0;
	bool useLzo = false;
	bool useCompressionFlag = false;

	// открывает файл целиком в память
	bool open(const char* path)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in) {
			return false;
		}
		in.seekg(0, std::ios::end);
		std::streamoff sz = in.tellg();
		in.seekg(0, std::ios::beg);
		if (sz < 0) {
			return false;
		}
		data.resize(static_cast<size_t>(sz));
		in.read(reinterpret_cast<char*>(data.data()), sz);
		return in.gcount() == sz;
	}

	size_t size() const { return data.size(); }
	size_t remaining() const { return data.size() - pos; }

	// проверка, что позиция в пределах файла
	void need(size_t n) const
	{
		if (pos + n > data.size()) {
			throw std::runtime_error("чтение за концом файла @ " + std::to_string(pos));
		}
	}

	uint8_t u8()
	{
		need(1);
		return data[pos++];
	}

	bool boolean() { return u8() != 0; }

	uint16_t u16()
	{
		need(2);
		uint16_t v = static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
		pos += 2;
		return v;
	}

	uint32_t u32()
	{
		need(4);
		uint32_t v = static_cast<uint32_t>(data[pos]) | (static_cast<uint32_t>(data[pos + 1]) << 8) | (static_cast<uint32_t>(data[pos + 2]) << 16) | (static_cast<uint32_t>(data[pos + 3]) << 24);
		pos += 4;
		return v;
	}

	int32_t i32() { return static_cast<int32_t>(u32()); }

	float f32()
	{
		uint32_t bits = u32();
		return std::bit_cast<float>(bits);
	}

	void skip(size_t n)
	{
		need(n);
		pos += n;
	}

	// пик значения без чтения
	uint32_t peekU32(size_t at) const
	{
		if (at + 4 > data.size()) {
			throw std::runtime_error("peek за концом файла");
		}
		return static_cast<uint32_t>(data[at]) | (static_cast<uint32_t>(data[at + 1]) << 8) | (static_cast<uint32_t>(data[at + 2]) << 16) | (static_cast<uint32_t>(data[at + 3]) << 24);
	}

	// hex-выдержка из позиции для диагностики ошибок чтения
	std::string hexAt(size_t at) const
	{
		std::string s;
		for (size_t i = at; i < data.size() && i < at + 24; i++) {
			char buf[4];
			std::snprintf(buf, sizeof(buf), "%02X ", data[i]);
			s += buf;
		}
		return s;
	}

	// ascii-строка фиксированной длины с санитизацией байтов старше 127
	std::string ascii(size_t n)
	{
		need(n);
		std::string s;
		for (size_t i = 0; i < n; i++) {
			uint8_t b = data[pos + i];
			s.push_back(b > 127 ? '?' : static_cast<char>(b));
		}
		pos += n;
		return s;
	}

	// asciiz-строка (до нулевого байта)
	std::string asciiz()
	{
		std::string s;
		for (;;) {
			uint8_t b = u8();
			if (b == 0) {
				break;
			}
			s.push_back(b > 127 ? '?' : static_cast<char>(b));
		}
		return s;
	}

	// сжатый блок LZO или LZSS (или сырой) размером expected байт
	std::vector<uint8_t> readCompressed(uint32_t expected)
	{
		if (expected == 0) {
			return {};
		}
		if (useLzo) {
			return readLzo(expected);
		}
		return readLzss(expected);
	}

	// блок LZO: после v64 признак сжатия явный байтом, раньше - сжимается всё >= 1024
	std::vector<uint8_t> readLzo(uint32_t expected)
	{
		bool compressed = expected >= 1024;
		if (useCompressionFlag) {
			compressed = boolean();
		}
		if (!compressed) {
			need(expected);
			std::vector<uint8_t> raw(data.begin() + (long)pos, data.begin() + (long)pos + expected);
			pos += expected;
			return raw;
		}
		std::vector<uint8_t> out(expected);
		size_t consumed = lzo1xDecompress(data.data() + pos, data.size() - pos, out.data(), expected);
		pos += consumed;
		return out;
	}

	// блок LZSS: маленькие блоки до 1024 байт пишутся сырыми
	std::vector<uint8_t> readLzss(uint32_t expected)
	{
		if (expected < 1024) {
			need(expected);
			std::vector<uint8_t> raw(data.begin() + (long)pos, data.begin() + (long)pos + expected);
			pos += expected;
			return raw;
		}
		std::vector<uint8_t> out(expected);
		size_t consumed = lzssDecompress(data.data() + pos, data.size() - pos, out.data(), expected);
		pos += consumed;
		return out;
	}

	// массив элементов фиксированного размера через сжатый блок
	template <typename T, typename F>
	std::vector<T> readCompressedArray(F&& readElem, size_t elemSize)
	{
		int32_t count = i32();
		if (count < 0 || static_cast<uint64_t>(count) > 10000000ULL) {
			throw std::runtime_error("подозрительный размер сжатого массива: " + std::to_string(count) + " @ " + std::to_string(pos - 4) + " [" + hexAt(pos - 4) + "]");
		}
		std::vector<uint8_t> buf = readCompressed(static_cast<uint32_t>(static_cast<uint64_t>(count) * elemSize));
		Reader sub;
		sub.data = std::move(buf);
		sub.version = version;
		sub.useLzo = useLzo;
		sub.useCompressionFlag = useCompressionFlag;
		std::vector<T> out;
		out.reserve(static_cast<size_t>(count));
		for (int32_t i = 0; i < count; i++) {
			out.push_back(readElem(sub));
		}
		return out;
	}

	// конденсированный массив: либо один общий элемент, либо сжатый блок
	template <typename T, typename F>
	std::vector<T> readCondensedArray(F&& readElem, size_t elemSize)
	{
		int32_t count = i32();
		if (count < 0 || static_cast<uint64_t>(count) > 10000000ULL) {
			throw std::runtime_error("подозрительный размер конденсированного массива: " + std::to_string(count));
		}
		if (boolean()) {
			T once = readElem(*this);
			return std::vector<T>(static_cast<size_t>(count), once);
		}
		std::vector<uint8_t> buf = readCompressed(static_cast<uint32_t>(static_cast<uint64_t>(count) * elemSize));
		Reader sub;
		sub.data = std::move(buf);
		sub.version = version;
		sub.useLzo = useLzo;
		sub.useCompressionFlag = useCompressionFlag;
		std::vector<T> out;
		out.reserve(static_cast<size_t>(count));
		for (int32_t i = 0; i < count; i++) {
			out.push_back(readElem(sub));
		}
		return out;
	}

	// простой массив (без сжатия): счётчик + сырые данные
	template <typename T, typename F>
	std::vector<T> readPlainArray(F&& readElem)
	{
		int32_t count = i32();
		if (count < 0 || static_cast<uint64_t>(count) > remaining()) {
			throw std::runtime_error("подозрительный размер массива: " + std::to_string(count) + " @ " + std::to_string(pos - 4) + " [" + hexAt(pos - 4) + "]");
		}
		std::vector<T> out;
		out.reserve(static_cast<size_t>(count));
		for (int32_t i = 0; i < count; i++) {
			out.push_back(readElem(*this));
		}
		return out;
	}

	// массив индексов вершин: u16 (до v68 включительно, 0xFFFF = -1) либо i32 (v69+)
	std::vector<int32_t> readVertexIndexArray()
	{
		if (version >= 69) {
			return readCompressedArray<int32_t>([](Reader& r) { return r.i32(); }, 4);
		}
		return readCompressedArray<int32_t>([](Reader& r) {
			uint16_t v = r.u16();
			return static_cast<int32_t>(v == 0xFFFF ? -1 : v);
		}, 2);
	}

	// пропускает массив индексов вершин - данные в конвертации не нужны
	void skipVertexIndexArray()
	{
		int32_t count = i32();
		if (count < 0 || static_cast<uint64_t>(count) > 10000000ULL) {
			throw std::runtime_error("подозрительный размер сжатого массива: " + std::to_string(count));
		}
		uint32_t elem = version >= 69 ? 4u : 2u;
		readCompressed(static_cast<uint32_t>(static_cast<uint64_t>(count) * elem));
	}

	// пропускает простой массив: счётчик плюс сырые данные
	void skipPlainArray(size_t elemSize)
	{
		int32_t count = i32();
		if (count < 0 || static_cast<uint64_t>(count) > remaining()) {
			throw std::runtime_error("подозрительный размер массива: " + std::to_string(count));
		}
		skip(static_cast<size_t>(count) * elemSize);
	}

	// пропускает сжатый массив: блок распаковывается и сразу отдаётся в никуда
	void skipCompressedArray(size_t elemSize)
	{
		int32_t count = i32();
		if (count < 0 || static_cast<uint64_t>(count) > 10000000ULL) {
			throw std::runtime_error("подозрительный размер сжатого массива: " + std::to_string(count));
		}
		readCompressed(static_cast<uint32_t>(static_cast<uint64_t>(count) * elemSize));
	}

	// пропускает вложенные массивы: снаружи счётчик объектов, у каждого свой массив
	void skipNestedIntArray()
	{
		int32_t outer = i32();
		if (outer < 0 || static_cast<uint64_t>(outer) > remaining()) {
			throw std::runtime_error("подозрительный размер массива: " + std::to_string(outer));
		}
		for (int32_t i = 0; i < outer; i++) {
			int32_t n = i32();
			if (n < 0 || static_cast<uint64_t>(n) > remaining()) {
				throw std::runtime_error("подозрительный размер массива: " + std::to_string(n));
			}
			skip(static_cast<size_t>(n) * 4);
		}
	}

	// один индекс вершины внутри полигона
	int32_t readVertexIndex()
	{
		if (version >= 69) {
			return i32();
		}
		uint16_t v = u16();
		return static_cast<int32_t>(v == 0xFFFF ? -1 : v);
	}

private:
		// распаковка LZO1X (state machine: literal run -> m1/m2/m3/m4 match -> хвостовые литералы)
	size_t lzo1xDecompress(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen)
	{
		const uint8_t* ip = in;
		const uint8_t* ipEnd = in + inLen;
		uint8_t* op = out;
		uint8_t* opEnd = out + outLen;
		const uint8_t* start = in;

		if (inLen < 3) {
			throw std::runtime_error("LZO: вход слишком короткий");
		}

		auto needIn = [&](size_t n) {
			if (static_cast<size_t>(ipEnd - ip) < n) {
				throw std::runtime_error("LZO: конец входа");
			}
		};
		auto needOut = [&](size_t n) {
			if (static_cast<size_t>(opEnd - op) < n) {
				throw std::runtime_error("LZO: переполнение выхода");
			}
		};
		// копирует n литералов из входа в выход
		auto copyLiterals = [&](uint32_t n) {
			needIn(n);
			needOut(n);
			for (uint32_t i = 0; i < n; i++) {
				*op++ = *ip++;
			}
		};
		// длинная длина: серия нулевых байтов плюс остаток
		auto longLength = [&](uint32_t base) {
			uint32_t n = base;
			while (*ip == 0) {
				n += 255;
				ip++;
			}
			return n + static_cast<uint32_t>(*ip++);
		};
		// копия матча из окна: смещение уже вычислено, длина len + 2
		auto copyMatch = [&](uint8_t* mPos, uint32_t len) {
			if (mPos < out || mPos >= op) {
				throw std::runtime_error("LZO: битый offset");
			}
			needOut(len + 2);
			*op++ = *mPos++;
			*op++ = *mPos++;
			for (uint32_t i = 0; i < len; i++) {
				*op++ = *mPos++;
			}
		};
		// один match-опкод, true когда пришёл маркер конца потока
		auto runMatch = [&](uint32_t t) -> bool {
			uint8_t* mPos;
			uint32_t len;
			if (t >= 64) {
				// m2: 11-битное окно, копия минимум 3 байта
				mPos = op - 1 - ((t >> 2) & 7) - (static_cast<size_t>(*ip++) << 3);
				len = (t >> 5) - 1;
			} else if (t >= 32) {
				// m3: 14-битное окно, длинная длина
				len = t & 0x1F;
				if (len == 0) {
					len = longLength(31);
				}
				mPos = op - 1 - ((ip[0] >> 2) + (static_cast<size_t>(ip[1]) << 6));
				ip += 2;
			} else if (t >= 16) {
				// m4: большое окно плюс смещение 16384
				mPos = op - ((t & 8) << 11);
				len = t & 7;
				if (len == 0) {
					len = longLength(7);
				}
				mPos -= (ip[0] >> 2) + (static_cast<size_t>(ip[1]) << 6);
				ip += 2;
				if (mPos == op) {
					// маркер конца: выход обязан быть заполнен целиком
					if (op != opEnd) {
						throw std::runtime_error("LZO: неожиданный конец потока");
					}
					return true;
				}
				mPos -= 16384;
			} else {
				// m1: 2 байта из ближнего окна
				mPos = op - 1 - (t >> 2) - (static_cast<size_t>(*ip++) << 2);
				copyMatch(mPos, 0);
				return false;
			}
			copyMatch(mPos, len);
			return false;
		};
		// хвостовые литералы после матча и следующий опкод; true = конец потока
		auto matchTail = [&]() -> bool {
			for (;;) {
				uint32_t lit = ip[-2] & 3;
				if (lit == 0) {
					return false;
				}
				copyLiterals(lit);
				if (runMatch(*ip++)) {
					return true;
				}
			}
		};
		// короткий матч после литерал-рана: 3 байта из окна 2048
		auto shortMatch = [&]() {
			uint32_t t = *ip++;
			uint8_t* mPos = op - 1 - (t >> 2) - (static_cast<size_t>(*ip++) << 2);
			if (mPos < out || mPos >= op) {
				throw std::runtime_error("LZO: битый offset");
			}
			needOut(3);
			*op++ = *mPos++;
			*op++ = *mPos++;
			*op++ = *mPos;
		};
		// вступ: первый байт больше 17 - это литералы в начале потока
		if (*ip > 17) {
			uint32_t t = static_cast<uint32_t>(*ip++ - 17);
			if (t < 4) {
				copyLiterals(t);
				if (runMatch(*ip++)) {
					return static_cast<size_t>(ip - start);
				}
				if (matchTail()) {
					return static_cast<size_t>(ip - start);
				}
			} else {
				copyLiterals(t);
				t = *ip++;
				if (t >= 16) {
					if (runMatch(t)) {
						return static_cast<size_t>(ip - start);
					}
					if (matchTail()) {
						return static_cast<size_t>(ip - start);
					}
				} else {
					shortMatch();
					if (matchTail()) {
						return static_cast<size_t>(ip - start);
					}
				}
			}
		}

		// главный цикл: литерал-ран, затем опкод матча
		for (;;) {
			uint32_t t = *ip++;
			if (t >= 16) {
				if (runMatch(t)) {
					return static_cast<size_t>(ip - start);
				}
				if (matchTail()) {
					return static_cast<size_t>(ip - start);
				}
				continue;
			}
			if (t == 0) {
				t = longLength(15);
			}
			needIn(t + 3);
			needOut(t + 3);
			*op++ = *ip++;
			*op++ = *ip++;
			*op++ = *ip++;
			for (uint32_t i = 0; i < t; i++) {
				*op++ = *ip++;
			}
			// после литерал-рана идёт опкод: либо обычный матч, либо короткий
			t = *ip++;
			if (t >= 16) {
				if (runMatch(t)) {
					return static_cast<size_t>(ip - start);
				}
				if (matchTail()) {
					return static_cast<size_t>(ip - start);
				}
				continue;
			}
			shortMatch();
			if (matchTail()) {
				return static_cast<size_t>(ip - start);
			}
		}
	}

// распаковка LZSS: окно 4096, стартовый индекс 4078, за данными 4 байта чексуммы
	size_t lzssDecompress(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen)
	{
		std::array<uint8_t, 4113> window{};
		std::fill(window.begin(), window.begin() + 4078, static_cast<uint8_t>(' '));
		const uint8_t* ip = in;
		const uint8_t* ipEnd = in + inLen;
		size_t outPos = 0;
		int wPos = 4078;
		int flags = 0;
		int32_t sum = 0;
		const uint8_t* start = in;

		while (outPos < outLen) {
			if (((flags >>= 1) & 0x100) == 0) {
				if (ip >= ipEnd) {
					throw std::runtime_error("LZSS: конец входа");
				}
				flags = *ip++ | 0xFF00;
			}
			if (flags & 1) {
				if (ip >= ipEnd) {
					throw std::runtime_error("LZSS: конец входа");
				}
				uint8_t b = *ip++;
				sum += static_cast<int32_t>(b);
				out[outPos++] = b;
				window[wPos] = b;
				wPos = (wPos + 1) & 0xFFF;
			} else {
				if (ip + 1 >= ipEnd) {
					throw std::runtime_error("LZSS: конец входа");
				}
				int a = *ip++;
				int b = *ip++;
				a |= (b & 0xF0) << 4;
				b &= 0x0F;
				b += 2;
				int from = wPos - a;
				int to = from + b;
				if (b + 1 > static_cast<int>(outLen - outPos)) {
					throw std::runtime_error("LZSS: переполнение");
				}
				for (int j = from; j <= to; j++) {
					uint8_t v = window[j & 0xFFF];
					sum += static_cast<int32_t>(static_cast<int8_t>(v));
					out[outPos++] = v;
					window[wPos] = v;
					wPos = (wPos + 1) & 0xFFF;
				}
			}
		}
		if (ip + 4 <= ipEnd) {
			int32_t stored = static_cast<int32_t>(static_cast<uint32_t>(ip[0]) | (static_cast<uint32_t>(ip[1]) << 8) | (static_cast<uint32_t>(ip[2]) << 16) | (static_cast<uint32_t>(ip[3]) << 24));
			ip += 4;
			if (stored != sum) {
				std::fprintf(stderr, "предупреждение: LZSS чексумма не совпала (файл %d, посчитано %d)\n", stored, sum);
			}
		}
		return static_cast<size_t>(ip - start);
	}
};

// ---- писалка ----

class Writer
{
public:
	std::vector<uint8_t> data;

	void u8(uint8_t v) { data.push_back(v); }

	void u32(uint32_t v)
	{
		data.push_back(static_cast<uint8_t>(v & 0xFF));
		data.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
		data.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
		data.push_back(static_cast<uint8_t>(v >> 24));
	}

	void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }

	void f32(float v)
	{
		u32(std::bit_cast<uint32_t>(v));
	}

	void boolean(bool v) { u8(v ? 1 : 0); }

	// ascii-строка фиксированной длины с нулевым дополнением
	void asciiFixed(const std::string& s, size_t n)
	{
		for (size_t i = 0; i < n; i++) {
			u8(i < s.size() ? static_cast<uint8_t>(s[i]) : 0);
		}
	}

	void asciiz(const std::string& s)
	{
		for (char c : s) {
			u8(static_cast<uint8_t>(c));
		}
		u8(0);
	}

	bool save(const char* path) const
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out) {
			return false;
		}
		out.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
		return out.good();
	}
};

// ---- математика и общие типы ----

struct Vec3
{
	float x = 0, y = 0, z = 0;

	Vec3() = default;
	Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

	void read(Reader& r)
	{
		x = r.f32();
		y = r.f32();
		z = r.f32();
	}

	void write(Writer& w) const
	{
		w.f32(x);
		w.f32(y);
		w.f32(z);
	}

	Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
	Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }

	float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }

	float length() const { return std::sqrt(dot(*this)); }

	float distance(const Vec3& o) const { return (*this - o).length(); }

	void normalize()
	{
		float len = length();
		x /= len;
		y /= len;
		z /= len;
	}

	bool near(const Vec3& o, float eps) const
	{
		return std::fabs(x - o.x) < eps && std::fabs(y - o.y) < eps && std::fabs(z - o.z) < eps;
	}
};

// упакованный в 32 бита вектор (3 x 10 бит со знаком, масштаб -1/511) - нормали и uv
struct Vec3Packed
{
	int32_t raw = 0;

	static Vec3 unpack(int32_t v)
	{
		int a = v & 0x3FF;
		int b = (v >> 10) & 0x3FF;
		int c = (v >> 20) & 0x3FF;
		if (a > 511) a -= 1024;
		if (b > 511) b -= 1024;
		if (c > 511) c -= 1024;
		return Vec3(static_cast<float>(a) * -0.0019569471f, static_cast<float>(b) * -0.0019569471f, static_cast<float>(c) * -0.0019569471f);
	}
};

// статусы point flags в MLOD
enum PointFlags : uint32_t {
	PF_NONE = 0,
	PF_ONLAND = 1,
	PF_UNDERLAND = 2,
	PF_ABOVELAND = 4,
	PF_KEEPLAND = 8,
	PF_NOLIGHT = 0x10,
	PF_AMBIENT = 0x20,
	PF_FULLLIGHT = 0x40,
	PF_HALFLIGHT = 0x80,
	PF_DECAL = 0x100,
	PF_VDECAL = 0x200,
	PF_NOFOG = 0x1000,
	PF_SKYFOG = 0x2000,
};

// битовые флаги клиппинга из ODOL
enum ClipFlags : uint32_t {
	CF_NONE = 0,
	CF_LAND_STEP = 0x100,
	CF_LAND_UNDER = 0x200,
	CF_LAND_ABOVE = 0x400,
	CF_LAND_KEEP = 0x800,
	CF_DECAL_STEP = 0x1000,
	CF_DECAL_VERTICAL = 0x2000,
	CF_FOG_STEP = 0x4000,
	CF_FOG_SKY = 0x8000,
	CF_USER_MASK = 0xFF00000,
	CF_USER_STEP = 0x100000,
};

// перенос флагов клиппинга ODOL в point flags MLOD
static uint32_t clipToPointFlags(uint32_t cf)
{
	uint32_t pf = PF_NONE;
	if (cf & CF_LAND_STEP) {
		pf |= PF_ONLAND;
	} else if (cf & CF_LAND_UNDER) {
		pf |= PF_UNDERLAND;
	} else if (cf & CF_LAND_ABOVE) {
		pf |= PF_ABOVELAND;
	} else if (cf & CF_LAND_KEEP) {
		pf |= PF_KEEPLAND;
	}
	if (cf & CF_DECAL_STEP) {
		pf |= PF_DECAL;
	} else if (cf & CF_DECAL_VERTICAL) {
		pf |= PF_VDECAL;
	}
	if (cf & 209715200u) {
		pf |= PF_NOLIGHT;
	} else if (cf & 212860928u) {
		pf |= PF_FULLLIGHT;
	} else if (cf & 211812352u) {
		pf |= PF_HALFLIGHT;
	} else if (cf & 210763776u) {
		pf |= PF_AMBIENT;
	}
	if (cf & CF_FOG_STEP) {
		pf |= PF_NOFOG;
	} else if (cf & CF_FOG_SKY) {
		pf |= PF_SKYFOG;
	}
	uint32_t user = (cf & CF_USER_MASK) / CF_USER_STEP;
	pf |= user << 16;
	return pf;
}

// матрица 3x3 (колонки: aside, up, dir)
struct Matrix3
{
	Vec3 cols[3];

	void read(Reader& r)
	{
		cols[0].read(r);
		cols[1].read(r);
		cols[2].read(r);
	}
};

// матрица 4x4 (ориентация + позиция) - трансформации proxy и stage
struct Matrix4
{
	Matrix3 orientation;
	Vec3 position;

	void read(Reader& r)
	{
		orientation.read(r);
		position.read(r);
	}
};

// ---- структуры ODOL ----

// скелет модели: имя, флаг дискретности, пары костей (имя, родитель)
struct OdolSkeleton
{
	std::string name;
	bool isDiscrete = false;
	std::vector<std::string> bones; // попарно: имя, родитель
	std::string pivotsNameObsolete;

	void read(Reader& r)
	{
		name = r.asciiz();
		if (name.empty()) {
			return;
		}
		if (r.version >= 23) {
			isDiscrete = r.boolean();
		}
		int32_t count = r.i32();
		bones.resize(static_cast<size_t>(count) * 2);
		for (int32_t i = 0; i < count * 2; i++) {
			bones[static_cast<size_t>(i)] = r.asciiz();
		}
		if (r.version > 40) {
			pivotsNameObsolete = r.asciiz();
		}
	}
};

// чтение байтов уровней LOD из modelInfo с учётом geometrySimple в v54+
static void readModelInfoLodBytes(Reader& r)
{
	int version = r.version;
	r.u8(); // память
	r.u8(); // геометрия
	if (version > 53) {
		r.u8(); // простая геометрия (v54+)
	}
	r.u8(); // геометрия огня
	r.u8(); // геометрия обзора
	r.u8(); // обзор пилота
	r.u8(); // обзор наводчика
	r.u8(); // геометрия сверху
	r.u8(); // обзор груза
	r.u8(); // точки земли
	r.u8(); // дороги
	r.u8(); // пути
	r.u8(); // хитпоинты
	r.u32(); // мин тень
}

// сведения о модели: габариты, масса, скелет, свойства
struct OdolModelInfo
{
	Vec3 boundingCenter;
	float mass = 0;
	OdolSkeleton skeleton;
	std::string propertyClass;
	std::string propertyDamage;

	void read(Reader& r, int nLods)
	{
		int version = r.version;
		r.i32(); // особое
		r.f32();
		r.f32();
		r.i32();
		r.i32();
		r.i32();
		Vec3 tmp;
		tmp.read(r); // центр прицела
		r.u32();
		r.u32();
		r.f32();
		tmp.read(r); // граница мин
		tmp.read(r); // граница макс
		if (version >= 70) r.f32();
		if (version >= 71) r.f32();
		if (version >= 52) {
			tmp.read(r); // виз граница мин
			tmp.read(r); // виз граница макс
		}
		boundingCenter.read(r);
		tmp.read(r); // центр геометрии
		tmp.read(r); // центр масс
		r.skip(36); // инерция
		r.boolean();
		r.boolean();
		r.boolean();
		r.boolean();
		if (version >= 73) r.boolean();
		if (version == 53 || version == 54 || version == 55) r.skip(5);
		if (version >= 42) r.skip(16);
		if (version >= 43) r.skip(8);
		if (version >= 33) r.boolean();
		if (version >= 37) {
			r.i32();
			r.boolean();
		}
		if (version >= 48) r.f32();
		r.boolean(); // анимирована
		if (version >= 56) {
			// в v56 после animated идёт блок из 9 байт перед скелетом
			r.skip(9);
		}
		if (g_trace) std::printf("[trace]   mi pre-skeleton @ %zu\n", r.pos);
		skeleton.read(r);
		if (g_trace) std::printf("[trace]   mi skeleton '%s' @ %zu\n", skeleton.name.c_str(), r.pos);
		r.u8(); // тип карты
		if (g_trace) std::printf("[trace]   mi massArray peek @ %zu = 0x%08X\n", r.pos, r.peekU32(r.pos));
		// массив масс по LOD в конвертацию не идёт, сама масса приходит отдельным полем
		r.skipCompressedArray(4);
		mass = r.f32();
		r.f32();
		r.f32();
		r.f32();
		if (version >= 72) r.f32();
		if (g_trace) std::printf("[trace]   mi mass=%g @ %zu\n", mass, r.pos);
		readModelInfoLodBytes(r);
		if (version >= 38) r.boolean(); // смешивание
		propertyClass = r.asciiz();
		propertyDamage = r.asciiz();
		r.boolean(); // частое свойство
		if (g_trace) std::printf("[trace]   mi props '%s' @ %zu\n", propertyClass.c_str(), r.pos);
		if (version >= 31) r.skip(4);
		if (version == 55 || version == 56) r.skip(1);
		if (version >= 57) {
			// таблицы предпочтительных LOD для теней
			r.skip(static_cast<size_t>(nLods) * 4 * 3);
		}
	}
};

struct OdolAnimClass
{
	uint32_t type = 0;
	std::string name;
	std::string source;
	// типы анимаций
	enum : uint32_t { ROTATION = 0, ROTATIONX, ROTATIONY, ROTATIONZ, TRANSLATION, TRANSLATIONX, TRANSLATIONY, TRANSLATIONZ, DIRECT = 8, HIDE = 9 };

	void read(Reader& r)
	{
		size_t start = r.pos;
		type = r.u32();
		if (g_trace) std::printf("[trace]   animclass type=%u @ %zu\n", type, start);
		name = r.asciiz();
		source = r.asciiz();
		r.f32(); // мин фаза
		r.f32(); // макс фаза
		r.f32(); // мин значение
		r.f32(); // макс значение
		// в v56 после maxValue сразу sourceAddress (проверено по angle1=1.57 у крышки)
		r.u32(); // адрес источника
		if (type <= ROTATIONZ) {
			r.skip(8); // углы
		} else if (type <= TRANSLATIONZ) {
			r.skip(8); // смещения
		} else if (type == DIRECT) {
			Vec3 a, b;
			a.read(r);
			b.read(r);
			r.f32();
			r.f32();
		} else if (type == HIDE) {
			r.f32(); // значение скрытия
			if (r.version == 55) {
				// пятый float есть только в v55, в v56 его уже нет
				r.f32();
			}
		} else {
			throw std::runtime_error("неизвестный тип анимации: " + std::to_string(type));
		}
	}
};

struct OdolAnimations
{
	std::vector<OdolAnimClass> classes;
	// для model.cfg нужны только классы анимаций, карты костей пропускаем
	void read(Reader& r)
	{
		classes = r.readPlainArray<OdolAnimClass>([](Reader& s) { OdolAnimClass c; c.read(s); return c; });
		if (g_trace) std::printf("[trace]   anim classes=%zu @ %zu\n", classes.size(), r.pos);
		int32_t nAnimLods = r.i32();
		if (g_trace) std::printf("[trace]   anim nAnimLods=%d @ %zu\n", nAnimLods, r.pos);
		for (int32_t i = 0; i < nAnimLods; i++) {
			uint32_t nBones = r.u32();
			for (uint32_t j = 0; j < nBones; j++) {
				uint32_t nAnims = r.u32();
				if (g_trace) std::printf("[trace]   anim bones[%u] anims=%u @ %zu\n", j, nAnims, r.pos);
				r.skip(static_cast<size_t>(nAnims) * 4);
			}
		}
		if (g_trace) std::printf("[trace]   anim bones2anims done @ %zu\n", r.pos);
		for (int32_t l = 0; l < nAnimLods; l++) {
			for (size_t m = 0; m < classes.size(); m++) {
				int32_t bone = r.i32();
				if (g_trace) std::printf("[trace]   anim2bone[%zu]=%d @ %zu\n", m, bone, r.pos);
				if (bone != -1 && classes[m].type != OdolAnimClass::DIRECT && classes[m].type != OdolAnimClass::HIDE) {
					Vec3 a, b;
					a.read(r);
					b.read(r);
				}
			}
		}
		if (g_trace) std::printf("[trace]   anim axis done @ %zu\n", r.pos);
	}
};

struct OdolEmbeddedMaterial
{
	std::string materialName;
	uint32_t matVersion = 0;

	void read(Reader& r)
	{
		materialName = r.asciiz();
		matVersion = r.u32();
		if (g_trace) std::printf("[trace]   mat '%s' v%u @ %zu\n", materialName.c_str(), matVersion, r.pos);
		if (r.version <= 53) {
			readLegacy(r);
		} else {
			readModern(r);
		}
		if (g_trace) std::printf("[trace]   mat done @ %zu\n", r.pos);
	}

	// формат ODOL v28..v53: цвета, +32 при v16, шейдеры сразу, fogMode всегда
	void readLegacy(Reader& r)
	{
		r.skip(16 * 6); // эмиссия, фон, диффуз, форс-диффуз, блик, копия блика
		if (matVersion >= 16) r.skip(32);
		r.f32(); // сила блика
		r.u32(); // пиксельный шейдер
		if (matVersion == 15) r.skip(40);
		r.u32(); // вершинный шейдер
		if (matVersion >= 16) r.skip(24);
		r.u32(); // основной свет
		r.u32(); // режим тумана
		if (matVersion == 3) r.skip(1);
		if (matVersion >= 6) {
			r.asciiz(); // файл поверхности
		}
		uint32_t nStages = 0;
		uint32_t nTexGens = 0;
		if (matVersion >= 4) {
			r.u32(); // флаги рендера
			r.u32(); // флаги отрисовки
		}
		if (matVersion > 6) {
			nStages = r.u32();
		}
		if (matVersion > 8) {
			nTexGens = r.u32();
		}
		if (g_trace) std::printf("[trace]   mat stages=%u texgens=%u @ %zu\n", nStages, nTexGens, r.pos);
		if (nStages > 64 || nTexGens > 64) {
			throw std::runtime_error("подозрительное число stage/texgen в материале");
		}
		if (matVersion < 8) {
			for (uint32_t i = 0; i < nStages; i++) {
				readStageTransform(r);
				readStageTexture(r);
			}
		} else {
			for (uint32_t i = 0; i < nStages; i++) {
				readStageTexture(r);
			}
			for (uint32_t i = 0; i < nTexGens; i++) {
				readStageTransform(r);
			}
		}
		if (matVersion >= 10) {
			readStageTexture(r);
		}
	}

	// формат ODOL v54+: смещения полей сдвинуты, hasSurface и renderFlags по-новому
	void readModern(Reader& r)
	{
		r.skip(16 * 6);
		if (matVersion >= 12) r.skip(16);
		if (matVersion >= 13) r.skip(16);
		r.f32(); // сила блика
		if (matVersion >= 14) r.skip(4);
		if (matVersion >= 15) r.skip(4);
		if (matVersion >= 17) r.skip(4);
		if (matVersion >= 18) r.skip(40);
		if (matVersion >= 20) r.skip(20);
		if (matVersion == 19) r.skip(16);
		r.u32(); // пиксельный шейдер
		r.u32(); // вершинный шейдер
		if (g_trace) std::printf("[trace]   mat shaders @ %zu\n", r.pos);
		if (matVersion >= 18) {
			r.u32(); // мусор
		}
		r.u32(); // основной свет
		int hasSurface = 0;
		if (matVersion >= 18) {
			hasSurface = r.u8();
		}
		if (matVersion >= 18 && hasSurface == 0) {
			r.u32(); // режим тумана
			r.pos -= 1; // последний байт откатывается - дальше идёт surfaceFile
		} else {
			r.skip(2);
		}
		r.asciiz(); // файл поверхности
		r.u32(); // флаги рендера
		if (hasSurface != 0) {
			r.u32(); // флаги отрисовки
		}
		uint32_t nStages = r.u32();
		uint32_t nTexGens = r.u32();
		if (g_trace) std::printf("[trace]   mat stages=%u texgens=%u @ %zu\n", nStages, nTexGens, r.pos);
		if (nStages > 64 || nTexGens > 64) {
			throw std::runtime_error("подозрительное число stage/texgen в материале");
		}
		// v56: в файле лежат все записи stage и все texgen (с нулевой)
		for (uint32_t i = 0; i < nStages; i++) {
			readStageTexture(r);
		}
		for (uint32_t i = 0; i < nTexGens; i++) {
			readStageTransform(r);
		}
		// v56: в файле лежат все записи stage и все texgen (с нулевой),
		// затем одна запись stageTI - без финальных 52 байт
		if (matVersion >= 10) {
			readStageTexture(r);
		}
	}

	// одна текстура stage: фильтр, путь, id, флаг мировой карты
	void readStageTexture(Reader& r)
	{
		if (matVersion >= 5) r.u32(); // фильтр текстуры
		r.asciiz(); // текстура
		if (matVersion >= 8) r.u32(); // номер стадии
		if (matVersion >= 11) r.boolean(); // мировая карта
	}

	// uv-трансформация stage: источник uv + матрица 4x4
	void readStageTransform(Reader& r)
	{
		r.u32(); // источник uv
		r.skip(48); // матрица 4х4
	}
};

struct OdolProxy
{
	std::string proxyModel;
	Matrix4 transformation;
	int32_t namedSelectionIndex = 0;
	Vec3 position() const { return transformation.position; }
	Vec3 up() const { return transformation.orientation.cols[1]; }
	Vec3 dir() const { return transformation.orientation.cols[2]; }

	void read(Reader& r)
	{
		proxyModel = r.asciiz();
		transformation.read(r);
		r.i32(); // id последовательности
		namedSelectionIndex = r.i32();
		r.i32(); // индекс кости
		if (r.version >= 40) {
			r.i32(); // индекс секции
		}
	}
};

struct OdolUvSet
{
	std::vector<float> uv; // по 2 float на вершину

	void read(Reader& r, uint32_t ver)
	{
		bool discretized = false;
		float minU = 0, minV = 0, maxU = 0, maxV = 0;
		if (ver >= 45) {
			discretized = true;
			minU = r.f32();
			minV = r.f32();
			maxU = r.f32();
			maxV = r.f32();
		}
		uint32_t nVertices = r.u32();
		bool defaultFill = r.boolean();
		uint32_t elem = ver >= 45 ? 4 : 8;
		float du = 0, dv = 0;
		std::vector<uint8_t> raw;
		if (defaultFill) {
			if (discretized) {
				int16_t su = static_cast<int16_t>(r.u16());
				int16_t sv = static_cast<int16_t>(r.u16());
				du = scaleDiscrete(su, maxU - minU, minU);
				dv = scaleDiscrete(sv, maxV - minV, minV);
			} else {
				// defaultFill хранит сырые 8 байт (два float)
				du = std::bit_cast<float>(r.u32());
				dv = std::bit_cast<float>(r.u32());
			}
		} else {
			raw = r.readCompressed(nVertices * elem);
		}
		uv.resize(static_cast<size_t>(nVertices) * 2);
		for (uint32_t i = 0; i < nVertices; i++) {
			if (defaultFill) {
				uv[i * 2] = du;
				uv[i * 2 + 1] = dv;
			} else if (discretized) {
				size_t j = static_cast<size_t>(i) * 4;
				int16_t su = static_cast<int16_t>(static_cast<uint16_t>(raw[j]) | (static_cast<uint16_t>(raw[j + 1]) << 8));
				int16_t sv = static_cast<int16_t>(static_cast<uint16_t>(raw[j + 2]) | (static_cast<uint16_t>(raw[j + 3]) << 8));
				uv[i * 2] = scaleDiscrete(su, maxU - minU, minU);
				uv[i * 2 + 1] = scaleDiscrete(sv, maxV - minV, minV);
			} else {
				size_t j = static_cast<size_t>(i) * 8;
				uv[i * 2] = std::bit_cast<float>(packU32(&raw[j]));
				uv[i * 2 + 1] = std::bit_cast<float>(packU32(&raw[j + 4]));
			}
		}
	}

private:
	// восстановление дискретизованного uv из 16 бит
	static float scaleDiscrete(int16_t v, double scale, float minV)
	{
		return static_cast<float>(1.52587890625e-05 * static_cast<double>(v + 32767) * scale) + minV;
	}
};

struct OdolSection
{
	int32_t faceLowerIndex = 0;
	int32_t faceUpperIndex = 0;
	int16_t textureIndex = -1;
	int32_t materialIndex = -1;
	bool shortIndices = true;

	void read(Reader& r)
	{
		shortIndices = r.version < 69;
		faceLowerIndex = r.i32();
		faceUpperIndex = r.i32();
		r.i32(); // мин индекс кости
		r.i32(); // кол-во костей
		r.u32();
		textureIndex = static_cast<int16_t>(r.u16());
		r.u32(); // особое
		materialIndex = r.i32();
		if (materialIndex == -1) {
			r.asciiz(); // inline материал
		}
		if (r.version >= 36) {
			uint32_t nStages = r.u32();
			r.skip(static_cast<size_t>(nStages) * 4);
			if (r.version >= 67) {
				int32_t extra = r.i32();
				if (extra >= 1) {
					r.skip(11 * 4);
				}
			}
		} else {
			r.f32();
		}
	}

	// сопоставление секции с полигонами по бегущему байтовому смещению в face-потоке
	std::vector<uint32_t> faceIndexes(const std::vector<std::vector<int32_t>>& faces) const
	{
		uint32_t acc = 0;
		uint32_t step = shortIndices ? 8u : 16u;
		uint32_t extra = shortIndices ? 2u : 4u;
		std::vector<uint32_t> out;
		for (uint32_t i = 0; i < faces.size(); i++) {
			if (acc >= static_cast<uint32_t>(faceLowerIndex) && acc < static_cast<uint32_t>(faceUpperIndex)) {
				out.push_back(i);
			}
			acc += step;
			if (faces[i].size() == 4) {
				acc += extra;
			}
			if (acc >= static_cast<uint32_t>(faceUpperIndex)) {
				break;
			}
		}
		return out;
	}
};

struct OdolNamedSelection
{
	std::string name;
	bool isSectional = false;
	std::vector<int32_t> selectedFaces;
	std::vector<int32_t> sections;
	std::vector<int32_t> selectedVertices;
	std::vector<uint8_t> selectedVerticesWeights;

	void read(Reader& r)
	{
		name = r.asciiz();
		selectedFaces = r.readVertexIndexArray();
		r.u32();
		isSectional = r.boolean();
		sections = r.readPlainArray<int32_t>([](Reader& s) { return s.i32(); });
		selectedVertices = r.readVertexIndexArray();
		uint32_t weightsSize = r.u32();
		selectedVerticesWeights = r.readCompressed(weightsSize);
	}
};

struct OdolKeyframe
{
	float time = 0;
	std::vector<Vec3> points;

	void read(Reader& r)
	{
		time = r.f32();
		uint32_t count = r.u32();
		points.resize(count);
		for (uint32_t i = 0; i < count; i++) {
			points[i].read(r);
		}
	}
};

struct OdolLod
{
	float resolution = 0;
	std::vector<OdolProxy> proxies;
	uint32_t vertexCount = 0;
	Vec3 bMin, bMax, bCenter;
	float bRadius = 0;
	std::vector<std::string> textures;
	std::vector<OdolEmbeddedMaterial> materials;
	std::vector<std::vector<int32_t>> faces;
	std::vector<OdolSection> sections;
	std::vector<OdolNamedSelection> namedSelections;
	std::vector<std::pair<std::string, std::string>> namedProperties;
	std::vector<OdolKeyframe> frames;
	std::vector<int32_t> clip;
	std::vector<OdolUvSet> uvSets;
	std::vector<Vec3> vertices;
	std::vector<Vec3> normals;
	std::vector<std::pair<float, float>> stCoords;
	std::vector<int32_t> subSkeletonsToSkeleton;
	std::vector<std::vector<uint8_t>> vertexBoneRefRaw; // сырые 12 байт на вершину
	bool vertexBoneRefHasData = false;

	void read(Reader& r, float res)
	{
		int version = r.version;
		resolution = res;
		proxies = r.readPlainArray<OdolProxy>([](Reader& s) { OdolProxy p; p.read(s); return p; });
		if (g_trace) std::printf("[trace]   proxies=%zu @ %zu\n", proxies.size(), r.pos);
		subSkeletonsToSkeleton = r.readPlainArray<int32_t>([](Reader& s) { return s.i32(); });
		if (g_trace) std::printf("[trace]   subSkel=%zu @ %zu\n", subSkeletonsToSkeleton.size(), r.pos);
		// наборы подскелетов в MLOD не уходят, поток только пропускается
		r.skipNestedIntArray();
		if (g_trace) std::printf("[trace]   skeletonToSub @ %zu\n", r.pos);
		if (version >= 50) {
			vertexCount = r.u32();
		}
		if (version >= 51) {
			r.f32(); // площадь граней
		}
		r.i32(); // подсказки или
		r.i32(); // подсказки и
		bMin.read(r);
		bMax.read(r);
		bCenter.read(r);
		bRadius = r.f32();
		if (g_trace) std::printf("[trace]   vertexCount=%u @ %zu\n", vertexCount, r.pos);
		textures = r.readPlainArray<std::string>([](Reader& s) { return s.asciiz(); });
		if (g_trace) std::printf("[trace]   textures=%zu @ %zu\n", textures.size(), r.pos);
		int32_t nMaterials = r.i32();
		if (g_trace) std::printf("[trace]   nMaterials=%d @ %zu\n", nMaterials, r.pos);
		materials.resize(static_cast<size_t>(nMaterials));
		for (int32_t i = 0; i < nMaterials; i++) {
			materials[static_cast<size_t>(i)].read(r);
		}
		if (g_trace) std::printf("[trace]   materials done @ %zu\n", r.pos);
		// пары point<->vertex в конвертации не нужны
		r.skipVertexIndexArray();
		r.skipVertexIndexArray();
		if (g_trace) std::printf("[trace]   vtx-index arrays done @ %zu\n", r.pos);
		// полигоны
		uint32_t nFaces = r.u32();
		r.u32();
		r.u16();
		if (g_trace) std::printf("[trace]   nFaces=%u @ %zu\n", nFaces, r.pos);
		faces.resize(nFaces);
		for (uint32_t i = 0; i < nFaces; i++) {
			uint8_t n = r.u8();
			if (n < 3 || n > 8) {
				throw std::runtime_error("битое число вершин полигона: " + std::to_string(n));
			}
			faces[i].resize(n);
			for (uint8_t k = 0; k < n; k++) {
				faces[i][k] = r.readVertexIndex();
			}
		}
		sections = r.readPlainArray<OdolSection>([](Reader& s) { OdolSection sec; sec.read(s); return sec; });
		if (g_trace) std::printf("[trace]   sections=%zu @ %zu\n", sections.size(), r.pos);
		namedSelections = r.readPlainArray<OdolNamedSelection>([](Reader& s) { OdolNamedSelection ns; ns.read(s); return ns; });
		if (g_trace) std::printf("[trace]   namedSel=%zu @ %zu\n", namedSelections.size(), r.pos);
		if (vertexCount == 0 && faces.empty() && sections.empty() && namedSelections.empty()) {
			// пустой LOD - остальное не читается
			return;
		}
		uint32_t nProps = r.u32();
		if (nProps > 4096 || static_cast<uint64_t>(nProps) * 2 > r.remaining()) {
			nProps = 0;
		}
		namedProperties.reserve(nProps);
		for (uint32_t i = 0; i < nProps; i++) {
			std::string k = r.asciiz();
			std::string v = r.asciiz();
			namedProperties.emplace_back(k, v);
		}
		frames = r.readPlainArray<OdolKeyframe>([](Reader& s) { OdolKeyframe kf; kf.read(s); return kf; });
		r.i32(); // цвет верха
		r.i32(); // цвет
		r.i32(); // особое
		r.boolean(); // простые веса вершин
		r.u32(); // размер остаточных данных
		if (version >= 50) {
			clip = r.readCondensedArray<int32_t>([](Reader& s) { return s.i32(); }, 4);
		}
		// uv-наборы
		OdolUvSet uv0;
		uv0.read(r, static_cast<uint32_t>(version));
		uint32_t nUvSets = r.u32();
		if (nUvSets > 64) {
			throw std::runtime_error("подозрительное число uv-наборов: " + std::to_string(nUvSets));
		}
		uvSets.resize(nUvSets);
		if (nUvSets > 0) {
			uvSets[0] = std::move(uv0);
			for (uint32_t i = 1; i < nUvSets; i++) {
				uvSets[i].read(r, static_cast<uint32_t>(version));
			}
		}
		// вершины
		vertices = r.readCompressedArray<Vec3>([](Reader& s) { Vec3 v; v.read(s); return v; }, 12);
		// нормали
		if (version >= 45) {
			std::vector<int32_t> packed = r.readCondensedArray<int32_t>([](Reader& s) { return s.i32(); }, 4);
			normals.resize(packed.size());
			for (size_t i = 0; i < packed.size(); i++) {
				normals[i] = Vec3Packed::unpack(packed[i]);
			}
		} else {
			normals = r.readCondensedArray<Vec3>([](Reader& s) { Vec3 v; v.read(s); return v; }, 12);
		}
		// st-координаты (упакованные как два нормаль-пакинга со своим масштабом)
		if (version >= 45) {
			struct StRead {
				std::pair<float, float> operator()(Reader& s) const
				{
					int32_t a = s.i32();
					int32_t b = s.i32();
					auto unpack10 = [](int32_t v) {
						int x = v & 0x3FF;
						if (x > 511) x -= 1024;
						return x;
					};
					return { static_cast<float>(unpack10(a)) * (1.0f / 511.0f), static_cast<float>(unpack10(b)) * (1.0f / 511.0f) };
				}
			};
			stCoords = r.readCompressedArray<std::pair<float, float>>(StRead(), 8);
		} else {
			struct StRead24 {
				std::pair<float, float> operator()(Reader& s) const
				{
					return { s.f32(), s.f32() };
				}
			};
			stCoords = r.readCompressedArray<std::pair<float, float>>(StRead24(), 24);
		}
		// веса костей на вершину: по 12 байт (i32 + 8 байт)
		vertexBoneRefRaw = r.readCompressedArray<std::vector<uint8_t>>([](Reader& s) {
			int32_t n = s.i32();
			(void)n;
			std::vector<uint8_t> raw(8);
			for (int i = 0; i < 8; i++) raw[static_cast<size_t>(i)] = s.u8();
			return raw;
		}, 12);
		vertexBoneRefHasData = !vertexBoneRefRaw.empty();
		// соседние пары весов по 32 байта в MLOD не переносятся
		r.skipCompressedArray(32);
		if (version >= 67) {
			r.skip(4);
		}
		if (version >= 68) {
			r.skip(1);
		}
	}
};

struct OdolFile
{
	uint32_t version = 0;
	int32_t nLods = 0;
	std::vector<float> resolutions;
	OdolModelInfo modelInfo;
	bool hasAnims = false;
	OdolAnimations animations;
	std::vector<OdolLod> lods;

	void read(const char* path)
	{
		Reader r;
		if (!r.open(path)) {
			throw std::runtime_error(std::string("не удалось открыть файл: ") + path);
		}
		std::string sig = r.ascii(4);
		if (sig != "ODOL") {
			throw std::runtime_error("это не ODOL p3d (магия: " + sig + ")");
		}
		version = r.u32();
		std::printf("версия ODOL: %u\n", version);
		if (version < 28 || version > 73) {
			throw std::runtime_error("неподдерживаемая версия ODOL: " + std::to_string(version));
		}
		r.version = static_cast<int>(version);
		if (version >= 44) {
			r.useLzo = true;
			std::printf("сжатие: LZO\n");
		} else {
			std::printf("сжатие: LZSS\n");
		}
		if (version >= 64) {
			r.useCompressionFlag = true;
		}
		if (version >= 59) {
			r.u32(); // id приложения
		}
		if (version >= 58) {
			r.asciiz(); // дульная вспышка
		}
		nLods = r.i32();
		std::printf("LOD: %d\n", nLods);
		resolutions.resize(static_cast<size_t>(nLods));
		for (int32_t i = 0; i < nLods; i++) {
			resolutions[static_cast<size_t>(i)] = r.f32();
		}
		modelInfo.read(r, nLods);
		if (g_trace) std::printf("[trace] modelInfo done @ %zu\n", r.pos);
		if (version > 29) {
			hasAnims = r.boolean();
			if (g_trace) std::printf("[trace] hasAnims=%d @ %zu\n", static_cast<int>(hasAnims), r.pos);
			if (hasAnims) {
				traceMsg("чтение анимаций");
				animations.read(r);
				if (g_trace) std::printf("[trace] animations done @ %zu\n", r.pos);
			}
		}
		std::vector<uint32_t> lodStarts(static_cast<size_t>(nLods));
		std::vector<uint32_t> lodEnds(static_cast<size_t>(nLods));
		std::vector<bool> permanent(static_cast<size_t>(nLods));
		for (int32_t i = 0; i < nLods; i++) lodStarts[static_cast<size_t>(i)] = r.u32();
		for (int32_t i = 0; i < nLods; i++) lodEnds[static_cast<size_t>(i)] = r.u32();
		for (int32_t i = 0; i < nLods; i++) permanent[static_cast<size_t>(i)] = r.boolean();
		if (g_trace) {
			for (int32_t i = 0; i < nLods; i++) {
				std::printf("[trace] lod %d: start=%u end=%u perm=%d\n", i, lodStarts[static_cast<size_t>(i)], lodEnds[static_cast<size_t>(i)], static_cast<int>(permanent[static_cast<size_t>(i)]));
			}
		}
		lods.resize(static_cast<size_t>(nLods));
		size_t savedPos = r.pos;
		for (int32_t i = 0; i < nLods; i++) {
			if (!permanent[static_cast<size_t>(i)]) {
				// loadable LOD info: пропускаем по описанию
				r.i32(); // кол-во граней
				r.u32(); // цвет
				r.i32(); // особое
				r.u32(); // подсказки или
				if (version >= 39) r.boolean();
				if (version >= 51) {
					r.i32();
					r.f32();
				}
				savedPos = r.pos;
			}
			r.pos = lodStarts[static_cast<size_t>(i)];
			if (g_trace) {
				std::printf("[trace] LOD %d @ %u (res %g)\n", i, lodStarts[static_cast<size_t>(i)], resolutions[static_cast<size_t>(i)]);
			}
			lods[static_cast<size_t>(i)].read(r, resolutions[static_cast<size_t>(i)]);
			r.pos = savedPos;
		}
		if (version > 54 && !lodEnds.empty()) {
			uint32_t maxEnd = 0;
			for (uint32_t e : lodEnds) {
				if (e > maxEnd) maxEnd = e;
			}
			if (static_cast<size_t>(maxEnd) + 4 <= r.size()) {
				r.pos = maxEnd;
				r.i32(); // тип плавучести
			}
		}
		std::printf("чтение завершено, байтов прочитано: %zu из %zu\n", r.pos, r.size());
	}
};

// ---- структуры MLOD (запись) ----

struct MlodVertex
{
	int32_t point = 0;
	int32_t normal = 0;
	float u = 0, v = 0;
};

struct MlodFace
{
	std::vector<MlodVertex> verts; // 3 или 4
	std::string texture;
	std::string material;
	uint32_t flags = 0;
};

// типы тэгов MLOD
struct MlodTagg
{
	std::string name;
	// payload зависит от типа
	std::vector<uint8_t> points;   // named selection: вес точек
	std::vector<uint8_t> faces;    // named selection: признаки граней
	std::string propName;
	std::string propValue;
	std::vector<float> mass;
	std::vector<std::vector<float>> faceUVs; // uvset: по 2 float на вершину грани
	uint32_t uvSetNr = 0;
	float animTime = 0;
	std::vector<Vec3> animPoints;
};

struct MlodLod
{
	float resolution = 0;
	std::vector<Vec3> points;
	std::vector<uint32_t> pointFlags;
	std::vector<Vec3> normals;
	std::vector<MlodFace> faces;
	std::vector<MlodTagg> taggs;

	void write(Writer& w) const
	{
		w.asciiFixed("P3DM", 4);
		w.u32(28);
		w.u32(256);
		w.u32(static_cast<uint32_t>(points.size()));
		w.u32(static_cast<uint32_t>(normals.size()));
		w.u32(static_cast<uint32_t>(faces.size()));
		w.u32(0); // не используется
		for (size_t i = 0; i < points.size(); i++) {
			points[i].write(w);
			w.u32(pointFlags[i]);
		}
		for (const Vec3& n : normals) {
			n.write(w);
		}
		for (const MlodFace& f : faces) {
			w.i32(static_cast<int32_t>(f.verts.size()));
			for (int k = 0; k < 4; k++) {
				if (k < static_cast<int>(f.verts.size())) {
					const MlodVertex& v = f.verts[static_cast<size_t>(k)];
					w.i32(v.point);
					w.i32(v.normal);
					w.f32(v.u);
					w.f32(v.v);
				} else {
					w.i32(0);
					w.i32(0);
					w.f32(0);
					w.f32(0);
				}
			}
			w.u32(f.flags);
			w.asciiz(f.texture);
			w.asciiz(f.material);
		}
		w.asciiFixed("TAGG", 4);
		for (const MlodTagg& tagg : taggs) {
			writeTagg(w, tagg);
		}
		w.boolean(true);
		w.asciiz("#EndOfFile#");
		w.u32(0);
		w.f32(resolution);
	}

private:
	// запись одного тэга с заголовком
	static void writeTagg(Writer& w, const MlodTagg& tagg)
	{
		w.boolean(true);
		w.asciiz(tagg.name);
		if (tagg.name == "#Property#") {
			w.u32(128);
			w.asciiFixed(tagg.propName, 64);
			w.asciiFixed(tagg.propValue, 64);
		} else if (tagg.name == "#Mass#") {
			w.u32(static_cast<uint32_t>(tagg.mass.size() * 4));
			for (float m : tagg.mass) w.f32(m);
		} else if (tagg.name == "#UVSet#") {
			uint32_t size = 4;
			for (const auto& fu : tagg.faceUVs) {
				size += static_cast<uint32_t>(fu.size()) * 4;
			}
			w.u32(size);
			w.u32(tagg.uvSetNr);
			for (const auto& fu : tagg.faceUVs) {
				for (float v : fu) w.f32(v);
			}
		} else if (tagg.name == "#Animation#") {
			w.u32(static_cast<uint32_t>(tagg.animPoints.size() * 12 + 4));
			w.f32(tagg.animTime);
			for (const Vec3& p : tagg.animPoints) p.write(w);
		} else {
			// named selection: имя точек + имя граней
			w.u32(static_cast<uint32_t>(tagg.points.size() + tagg.faces.size()));
			for (uint8_t b : tagg.points) w.u8(b);
			for (uint8_t b : tagg.faces) w.u8(b);
		}
	}
};

struct MlodFile
{
	std::vector<MlodLod> lods;

	bool save(const char* path) const
	{
		Writer w;
		w.asciiFixed("MLOD", 4);
		w.u32(257);
		w.u32(static_cast<uint32_t>(lods.size()));
		for (const MlodLod& lod : lods) {
			lod.write(w);
		}
		return w.save(path);
	}
};

// ---- конвертация ODOL -> MLOD ----

// LOD, для которых именованные выделения переносятся как есть (без реконструкции из секций)
static bool keepsNamedSelections(float r)
{
	if (r == 1e15f || r == 7e15f || r == 1e13f || r == 6e15f || r == 1.3e16f || r == 1.5e16f || r == 8e15f || r == 4e15f || r == 5e15f || r == 4e13f) {
		return true;
	}
	return r == 2e13f;
}

struct PointWeight
{
	int32_t pointIndex = 0;
	uint8_t weight = 0;
};

// конвертация одного LOD
static MlodLod convertLod(const OdolFile& odol, const OdolLod& src)
{
	MlodLod dst;
	dst.resolution = src.resolution;
	int vertexCount = static_cast<int>(src.vertices.size());
	// точки: вершины со сдвигом на центр габаритов и флагами клиппинга
	dst.points.resize(static_cast<size_t>(vertexCount));
	dst.pointFlags.resize(static_cast<size_t>(vertexCount));
	for (int i = 0; i < vertexCount; i++) {
		dst.points[static_cast<size_t>(i)] = src.vertices[static_cast<size_t>(i)] + odol.modelInfo.boundingCenter;
		uint32_t cf = static_cast<size_t>(i) < src.clip.size() ? static_cast<uint32_t>(src.clip[static_cast<size_t>(i)]) : 0;
		dst.pointFlags[static_cast<size_t>(i)] = clipToPointFlags(cf);
	}
	dst.normals = src.normals;
	// грани: идём по секциям, вершины в обратном порядке, uv из первого набора
	const std::vector<float> emptyUv;
	const std::vector<float>& uv0 = src.uvSets.empty() ? emptyUv : src.uvSets[0].uv;
	for (const OdolSection& sec : src.sections) {
		std::vector<uint32_t> idxs = sec.faceIndexes(src.faces);
		for (uint32_t fi : idxs) {
			const std::vector<int32_t>& poly = src.faces[fi];
			MlodFace face;
			int n = static_cast<int>(poly.size());
			face.verts.resize(static_cast<size_t>(n));
			for (int k = 0; k < n; k++) {
				int vi = poly[static_cast<size_t>(n - 1 - k)];
				MlodVertex& v = face.verts[static_cast<size_t>(k)];
				v.point = vi;
				v.normal = vi;
				if (vi >= 0 && static_cast<size_t>(vi) * 2 + 1 < uv0.size()) {
					v.u = uv0[static_cast<size_t>(vi) * 2];
					v.v = uv0[static_cast<size_t>(vi) * 2 + 1];
				}
			}
			if (sec.textureIndex >= 0 && static_cast<size_t>(sec.textureIndex) < src.textures.size()) {
				face.texture = src.textures[static_cast<size_t>(sec.textureIndex)];
			}
			if (sec.materialIndex >= 0 && static_cast<size_t>(sec.materialIndex) < src.materials.size()) {
				face.material = src.materials[static_cast<size_t>(sec.materialIndex)].materialName;
			}
			face.flags = 0;
			dst.faces.push_back(std::move(face));
		}
	}
	// тэги
	int nPoints = vertexCount;
	int nFaces = static_cast<int>(src.faces.size());
	if (src.resolution == 1e13f && odol.modelInfo.mass > 0 && nPoints > 0) {
		// масса по точкам геометрии
		MlodTagg tagg;
		tagg.name = "#Mass#";
		tagg.mass.resize(static_cast<size_t>(nPoints), odol.modelInfo.mass / static_cast<float>(nPoints));
		dst.taggs.push_back(std::move(tagg));
	}
	// uv-наборы
	for (size_t u = 0; u < src.uvSets.size(); u++) {
		MlodTagg tagg;
		tagg.name = "#UVSet#";
		tagg.uvSetNr = static_cast<uint32_t>(u);
		const std::vector<float>& uvd = src.uvSets[u].uv;
		tagg.faceUVs.resize(src.faces.size());
		for (size_t j = 0; j < src.faces.size(); j++) {
			const std::vector<int32_t>& poly = src.faces[j];
			int n = static_cast<int>(poly.size());
			tagg.faceUVs[j].resize(static_cast<size_t>(n) * 2);
			for (int k = 0; k < n; k++) {
				int vi = poly[static_cast<size_t>(n - 1 - k)];
				if (vi >= 0 && static_cast<size_t>(vi) * 2 + 1 < uvd.size()) {
					tagg.faceUVs[j][static_cast<size_t>(k) * 2] = uvd[static_cast<size_t>(vi) * 2];
					tagg.faceUVs[j][static_cast<size_t>(k) * 2 + 1] = uvd[static_cast<size_t>(vi) * 2 + 1];
				}
			}
		}
		dst.taggs.push_back(std::move(tagg));
	}
	// именованные свойства
	for (const auto& kv : src.namedProperties) {
		MlodTagg tagg;
		tagg.name = "#Property#";
		tagg.propName = kv.first;
		tagg.propValue = kv.second;
		dst.taggs.push_back(std::move(tagg));
	}
	// именованные выделения
	for (const OdolNamedSelection& ns : src.namedSelections) {
		MlodTagg tagg;
		tagg.name = ns.name;
		tagg.points.assign(static_cast<size_t>(nPoints), 0);
		tagg.faces.assign(static_cast<size_t>(nFaces), 0);
		bool hasWeights = !ns.selectedVerticesWeights.empty();
		size_t wi = 0;
		for (int32_t vi : ns.selectedVertices) {
			if (vi >= 0 && vi < nPoints) {
				uint8_t wgt = 1;
				if (hasWeights && wi < ns.selectedVerticesWeights.size()) {
					wgt = static_cast<uint8_t>(-ns.selectedVerticesWeights[wi]);
					wi++;
				}
				if (wgt != 0) {
					tagg.points[static_cast<size_t>(vi)] = wgt;
				}
			}
		}
		for (int32_t fi : ns.selectedFaces) {
			if (fi >= 0 && fi < nFaces) {
				tagg.faces[static_cast<size_t>(fi)] = 1;
			}
		}
		dst.taggs.push_back(std::move(tagg));
	}
	// кадры анимации
	for (const OdolKeyframe& kf : src.frames) {
		MlodTagg tagg;
		tagg.name = "#Animation#";
		tagg.animTime = kf.time;
		tagg.animPoints = kf.points;
		dst.taggs.push_back(std::move(tagg));
	}
	// для служебных LOD именованные выделения восстанавливаем из секций, proxy и костей
	if (!keepsNamedSelections(src.resolution)) {
		std::map<std::string, std::vector<PointWeight>> nsPoints;
		std::map<std::string, std::vector<int32_t>> nsFaces;
		// из секций
		for (const OdolNamedSelection& ns : src.namedSelections) {
			if (!ns.isSectional) {
				continue;
			}
			std::vector<int32_t>& faces = nsFaces[ns.name];
			std::vector<PointWeight>& points = nsPoints[ns.name];
			for (int32_t si : ns.sections) {
				if (si < 0 || static_cast<size_t>(si) >= src.sections.size()) {
					continue;
				}
				for (uint32_t fi : src.sections[static_cast<size_t>(si)].faceIndexes(src.faces)) {
					faces.push_back(static_cast<int32_t>(fi));
					for (int32_t vi : src.faces[fi]) {
						points.push_back({ vi, 255 });
					}
				}
			}
		}
		// из proxy-треугольников
		for (size_t i = 0; i < src.faces.size(); i++) {
			const std::vector<int32_t>& poly = src.faces[i];
			if (poly.size() != 3) {
				continue;
			}
			Vec3 v0 = src.vertices[static_cast<size_t>(poly[0])];
			Vec3 v1 = src.vertices[static_cast<size_t>(poly[1])];
			Vec3 v2 = src.vertices[static_cast<size_t>(poly[2])];
			// сортируем вершины по возрастанию расстояния от самой дальней - получаем гипотенузу как основание
			float d01 = v0.distance(v1);
			float d02 = v0.distance(v2);
			float d12 = v1.distance(v2);
			if (d01 > d02) { std::swap(v1, v2); std::swap(d01, d02); }
			if (d01 > d12) { std::swap(v0, v2); std::swap(d01, d12); }
			if (d02 > d12) { std::swap(v1, v2); std::swap(d02, d12); }
			Vec3 base0 = v0;
			Vec3 edge1 = v1 - v0;
			Vec3 edge2 = v2 - v0;
			edge1.normalize();
			edge2.normalize();
			// треугольник proxy - ортогональный
			if (std::fabs(edge2.dot(edge1)) > 0.05f) {
				continue;
			}
			for (const OdolProxy& proxy : src.proxies) {
				if (base0.near(proxy.position(), 0.05f) && edge1.near(proxy.dir(), 0.05f) && edge2.near(proxy.up(), 0.05f)) {
					if (proxy.namedSelectionIndex < 0 || static_cast<size_t>(proxy.namedSelectionIndex) >= src.namedSelections.size()) {
						continue;
					}
					const std::string& name = src.namedSelections[static_cast<size_t>(proxy.namedSelectionIndex)].name;
					if (nsFaces.count(name)) {
						continue;
					}
					nsFaces[name].push_back(static_cast<int32_t>(i));
					for (int32_t vi : poly) {
						nsPoints[name].push_back({ vi, 255 });
					}
					break;
				}
			}
		}
		// из костей скелета
		if (src.vertexBoneRefHasData && !odol.modelInfo.skeleton.bones.empty()) {
			for (size_t vi = 0; vi < src.vertexBoneRefRaw.size() && vi < static_cast<size_t>(vertexCount); vi++) {
				const std::vector<uint8_t>& raw = src.vertexBoneRefRaw[vi];
				int32_t nSmall = static_cast<int32_t>(packU32(raw.data()));
				for (int k = 0; k < nSmall && k < 4; k++) {
					uint8_t sel = raw[4 + static_cast<size_t>(k) * 2];
					uint8_t wgt = raw[4 + static_cast<size_t>(k) * 2 + 1];
					if (sel < src.subSkeletonsToSkeleton.size()) {
						int boneIdx = src.subSkeletonsToSkeleton[static_cast<size_t>(sel)];
						size_t nameIdx = static_cast<size_t>(boneIdx) * 2;
						if (nameIdx < odol.modelInfo.skeleton.bones.size()) {
							nsPoints[odol.modelInfo.skeleton.bones[nameIdx]].push_back({ static_cast<int32_t>(vi), wgt });
						}
					}
				}
			}
		}
		// применяем восстановленные данные к уже созданным тэгам
		for (MlodTagg& tagg : dst.taggs) {
			auto pit = nsPoints.find(tagg.name);
			if (pit != nsPoints.end()) {
				for (const PointWeight& pw : pit->second) {
					if (pw.pointIndex >= 0 && pw.pointIndex < nPoints) {
						uint8_t wgt = static_cast<uint8_t>(-pw.weight);
						if (wgt != 0) {
							tagg.points[static_cast<size_t>(pw.pointIndex)] = wgt;
						}
					}
				}
			}
			auto fit = nsFaces.find(tagg.name);
			if (fit != nsFaces.end()) {
				for (int32_t fi : fit->second) {
					if (fi >= 0 && fi < nFaces) {
						tagg.faces[static_cast<size_t>(fi)] = 1;
					}
				}
			}
		}
	}
	return dst;
}

// полная конвертация файла
static MlodFile convert(const OdolFile& odol)
{
	MlodFile out;
	out.lods.resize(odol.lods.size());
	for (size_t i = 0; i < odol.lods.size(); i++) {
		if (g_trace) {
			std::printf("[trace] конвертация LOD %zu\n", i);
		}
		out.lods[i] = convertLod(odol, odol.lods[i]);
		std::printf("LOD %zu (res %g): точек %zu, граней %zu, тэгов %zu\n", i, odol.lods[i].resolution,
			out.lods[i].points.size(), out.lods[i].faces.size(), out.lods[i].taggs.size());
	}
	return out;
}

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	std::string input;
	std::string output;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "-v") {
			g_trace = true;
		} else if (input.empty()) {
			input = a;
		} else if (output.empty()) {
			output = a;
		}
	}
	if (input.empty()) {
		std::printf("odol2mlod - конвертер DayZ/Arma p3d (ODOL) в редактируемый MLOD\n");
		std::printf("использование: odol2mlod <input.p3d> [output.p3d] [-v]\n");
		std::printf("output по умолчанию: <input>_mlod.p3d, -v - подробная трассировка чтения\n");
		return 1;
	}
	if (output.empty()) {
		std::string base = input;
		size_t dot = base.rfind('.');
		size_t slash = base.find_last_of("/\\");
		if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
			base = base.substr(0, dot);
		}
		output = base + "_mlod.p3d";
	}
	try {
		OdolFile odol;
		odol.read(input.c_str());
		MlodFile mlod = convert(odol);
		if (!mlod.save(output.c_str())) {
			std::fprintf(stderr, "не удалось записать: %s\n", output.c_str());
			return 1;
		}
		std::printf("готово: %s\n", output.c_str());
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "ошибка: %s\n", e.what());
		return 1;
	}
}
