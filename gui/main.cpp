#include "mainwindow.h"

#include <QApplication>
#include <QFileInfo>

// Первый аргумент - путь к p3d, такое окно сразу конвертирует файл
int main(int argc, char** argv)
{
	QApplication app(argc, argv);
	QApplication::setOrganizationName(QStringLiteral("odol2mlod"));
	QApplication::setApplicationName(QStringLiteral("odol2mlod-gui"));
	QApplication::setApplicationVersion(QStringLiteral("1.0"));

	QString initialFile;
	if (argc > 1) {
		QString arg = QString::fromLocal8Bit(argv[1]);
		if (QFileInfo::exists(arg) && arg.endsWith(QStringLiteral(".p3d"), Qt::CaseInsensitive)) {
			initialFile = arg;
		}
	}

	MainWindow w(initialFile);
	w.show();

	return app.exec();
}
