#pragma once

#include <QMainWindow>
#include <QProcess>

class QCheckBox;
class QDragEnterEvent;
class QDropEvent;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

class MainWindow : public QMainWindow {
	Q_OBJECT
public:
	explicit MainWindow(const QString& initialFile = QString(), QWidget* parent = nullptr);

protected:
	void dragEnterEvent(QDragEnterEvent* ev) override;
	void dropEvent(QDropEvent* ev) override;

private slots:
	void pickFile();
	void convert();
	void openResultDir();
	void onProcessReadyRead();
	void onProcessFinished(int code, QProcess::ExitStatus status);

private:
	void setFile(const QString& path);
	void log(const QString& s);
	QString converterPath() const;

	QLineEdit* m_file = nullptr;
	QCheckBox* m_verbose = nullptr;
	QPushButton* m_convert = nullptr;
	QPushButton* m_openDir = nullptr;
	QPlainTextEdit* m_log = nullptr;
	QProcess* m_process = nullptr;
	QString m_outputPath;
};
