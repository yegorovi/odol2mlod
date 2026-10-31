#include "mainwindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

// Путь к конвертеру рядом с gui-сборкой
QString MainWindow::converterPath() const
{
	return QCoreApplication::applicationDirPath() + QStringLiteral("/odol2mlod.exe");
}

MainWindow::MainWindow(const QString& initialFile, QWidget* parent)
	: QMainWindow(parent)
{
	setAcceptDrops(true);
	setWindowTitle(QStringLiteral("odol2mlod - конвертер ODOL в MLOD"));
	resize(720, 480);

	auto* central = new QWidget(this);
	auto* root = new QVBoxLayout(central);

	// строка выбора файла
	auto* fileRow = new QHBoxLayout;
	auto* fileLabel = new QLabel(QStringLiteral("Файл p3d:"), central);
	m_file = new QLineEdit(central);
	m_file->setReadOnly(true);
	m_file->setPlaceholderText(QStringLiteral("перетащи p3d сюда или нажми Обзор"));
	auto* pickBtn = new QPushButton(QStringLiteral("Обзор..."), central);
	fileRow->addWidget(fileLabel);
	fileRow->addWidget(m_file, 1);
	fileRow->addWidget(pickBtn);

	// кнопки запуска
	auto* actionRow = new QHBoxLayout;
	m_verbose = new QCheckBox(QStringLiteral("Подробный вывод"), central);
	m_convert = new QPushButton(QStringLiteral("Конвертировать"), central);
	m_openDir = new QPushButton(QStringLiteral("Открыть папку результата"), central);
	m_convert->setEnabled(false);
	m_openDir->setEnabled(false);
	actionRow->addWidget(m_verbose);
	actionRow->addStretch(1);
	actionRow->addWidget(m_convert);
	actionRow->addWidget(m_openDir);

	// лог работы
	m_log = new QPlainTextEdit(central);
	m_log->setReadOnly(true);
	m_log->setPlaceholderText(QStringLiteral("здесь будет вывод конвертера"));

	root->addLayout(fileRow);
	root->addLayout(actionRow);
	root->addWidget(m_log, 1);
	setCentralWidget(central);
	statusBar()->showMessage(QStringLiteral("готов"));

	m_process = new QProcess(this);
	m_process->setProcessChannelMode(QProcess::MergedChannels);
	connect(m_process, &QProcess::readyRead, this, &MainWindow::onProcessReadyRead);
	connect(m_process, QProcess::finished, this, &MainWindow::onProcessFinished);
	connect(pickBtn, &QPushButton::clicked, this, &MainWindow::pickFile);
	connect(m_convert, &QPushButton::clicked, this, &MainWindow::convert);
	connect(m_openDir, &QPushButton::clicked, this, &MainWindow::openResultDir);

	if (!QFileInfo::exists(converterPath())) {
		log(QStringLiteral("не нашёл odol2mlod.exe рядом с программой: %1").arg(converterPath()));
		m_convert->setEnabled(false);
	} else {
		log(QStringLiteral("odol2mlod готов, выбери p3d или перетащи его в окно"));
	}
	if (!initialFile.isEmpty()) {
		// файл пришёл аргументом - подставляем и сразу конвертируем
		setFile(initialFile);
		convert();
	}
}

void MainWindow::pickFile()
{
	QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Выбери p3d файл"), QString(),
	(QStringLiteral("Модели DayZ (*.p3d)")));
	if (path.isEmpty()) {
		return;
	}
	setFile(path);
}

void MainWindow::convert()
{
	if (m_process->state() != QProcess::NotRunning) {
		return;
	}
	QString input = m_file->text();
	if (input.isEmpty() || !QFileInfo::exists(input)) {
		log(QStringLiteral("сначала выбери p3d файл"));
		return;
	}
	if (!QFileInfo::exists(converterPath())) {
		log(QStringLiteral("нет odol2mlod.exe рядом с программой - собери его из odol2mlod.cpp"));
		return;
	}

	QString outPath = input;
	outPath.chop(4);
	outPath += QStringLiteral("_mlod.p3d");

	QStringList args;
	args << input << outPath;
	if (m_verbose->isChecked()) {
		args << QStringLiteral("-v");
	}
	m_outputPath = outPath;
	m_openDir->setEnabled(false);
	m_convert->setEnabled(false);
	m_log->clear();
	log(QStringLiteral("$ odol2mlod %1 %2%3").arg(QDir::toNativeSeparators(input),
		QDir::toNativeSeparators(outPath), m_verbose->isChecked() ? QStringLiteral(" -v") : QString()));
	statusBar()->showMessage(QStringLiteral("конвертирую..."));
	m_process->start(converterPath(), args);
	if (!m_process->waitForStarted(3000)) {
		log(QStringLiteral("не смог запустить конвертер"));
		m_convert->setEnabled(true);
		statusBar()->showMessage(QStringLiteral("ошибка запуска"));
	}
}

void MainWindow::openResultDir()
{
	QFileInfo info(m_outputPath);
	QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
}

void MainWindow::onProcessReadyRead()
{
	QByteArray data = m_process->readAll();
	log(QString::fromUtf8(data).trimmed());
}

void MainWindow::onProcessFinished(int code, QProcess::ExitStatus status)
{
	if (status == QProcess::NormalExit && code == 0 && QFileInfo::exists(m_outputPath)) {
		// строка готово уже пришла из вывода конвертера
		m_openDir->setEnabled(true);
		statusBar()->showMessage(QStringLiteral("готово"), 5000);
	} else {
		log(QStringLiteral("конвертер завершился с кодом %1").arg(code));
		statusBar()->showMessage(QStringLiteral("ошибка"), 5000);
	}
	m_convert->setEnabled(true);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* ev)
{
	if (ev->mimeData()->hasUrls()) {
		ev->acceptProposedAction();
	}
}

void MainWindow::dropEvent(QDropEvent* ev)
{
	const QList<QUrl> urls = ev->mimeData()->urls();
	if (urls.isEmpty()) {
		return;
	}
	QString path = urls.first().toLocalFile();
	if (path.endsWith(QStringLiteral(".p3d"), Qt::CaseInsensitive)) {
		setFile(path);
		ev->acceptProposedAction();
	}
}

void MainWindow::setFile(const QString& path)
{
	m_file->setText(path);
	m_convert->setEnabled(true);
	statusBar()->showMessage(QStringLiteral("файл выбран"), 3000);
}

void MainWindow::log(const QString& s)
{
	if (s.isEmpty()) {
		return;
	}
	m_log->appendPlainText(s);
	m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
}
