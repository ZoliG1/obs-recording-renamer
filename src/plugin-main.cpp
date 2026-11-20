/*
Recording Renamer for OBS Studio
Copyright (C) 2026 ZoliG1

After a recording stops, asks for a name and puts it in front of the file name:
     - PS4 - 19.11.2026 19-48-55.mp4
  -> Johnny hack - PS4 - 19.11.2026 19-48-55.mp4

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>
#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Asks for a name when a recording stops and puts it in front of the file name.";
}

namespace {

const char *DOCK_ID = "recording-renamer-scripts-dock";

struct Settings {
	bool enabled = true;
	bool useTimeout = true;
	int timeout = 20;
};

Settings g_settings;
QPointer<QCheckBox> g_dockCheck;

/* ------------------------------------------------------------ settings file */

std::string settingsPath()
{
	char *p = obs_module_config_path("settings.json");
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

void loadSettings()
{
	obs_data_t *d = obs_data_create_from_json_file_safe(settingsPath().c_str(), "bak");
	if (!d)
		return;
	obs_data_set_default_bool(d, "enabled", true);
	obs_data_set_default_bool(d, "use_timeout", true);
	obs_data_set_default_int(d, "timeout", 20);
	g_settings.enabled = obs_data_get_bool(d, "enabled");
	g_settings.useTimeout = obs_data_get_bool(d, "use_timeout");
	g_settings.timeout = (int)obs_data_get_int(d, "timeout");
	if (g_settings.timeout < 1)
		g_settings.timeout = 20;
	obs_data_release(d);
}

void saveSettings()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	obs_data_t *d = obs_data_create();
	obs_data_set_bool(d, "enabled", g_settings.enabled);
	obs_data_set_bool(d, "use_timeout", g_settings.useTimeout);
	obs_data_set_int(d, "timeout", g_settings.timeout);
	obs_data_save_json_safe(d, settingsPath().c_str(), "tmp", "bak");
	obs_data_release(d);
}

void setEnabled(bool on)
{
	g_settings.enabled = on;
	saveSettings();
	if (g_dockCheck && g_dockCheck->isChecked() != on) {
		QSignalBlocker block(g_dockCheck);
		g_dockCheck->setChecked(on);
	}
	obs_log(LOG_INFO, "Recording Renamer %s", on ? "enabled" : "disabled");
}

QWidget *mainWindow()
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}

/* ------------------------------------------------------------ renaming */

QString cleanName(const QString &text)
{
	static const QString invalid = QStringLiteral("<>:\"/\\|?*");
	QString out;
	for (const QChar c : text) {
		if (c.unicode() >= 32 && !invalid.contains(c))
			out += c;
	}
	out = out.trimmed();
	while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' ')))
		out.chop(1);
	return out;
}

QString uniqueTarget(const QFileInfo &fi, const QString &prefix)
{
	const QDir dir = fi.absoluteDir();
	const QString base = prefix + fi.completeBaseName();
	const QString ext = fi.suffix().isEmpty() ? QString() : QStringLiteral(".") + fi.suffix();

	QString target = dir.filePath(base + ext);
	for (int n = 2; QFileInfo::exists(target); n++)
		target = dir.filePath(QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(ext));
	return target;
}

/* The file can still be locked for a moment after recording stops, so retry
 * for up to ~20 seconds without blocking the OBS window. */
void renameWithRetry(const QString &from, const QString &to, int attempt)
{
	if (QFile::rename(from, to)) {
		obs_log(LOG_INFO, "Renamed recording to '%s'", to.toUtf8().constData());
		return;
	}
	if (attempt >= 40) {
		obs_log(LOG_WARNING, "Could not rename '%s'", from.toUtf8().constData());
		QMessageBox::warning(mainWindow(), QStringLiteral("Recording Renamer"),
				     QStringLiteral("Could not rename\n%1\n\nto\n%2\n\nThe file may be in use.")
					     .arg(QFileInfo(from).fileName(), QFileInfo(to).fileName()));
		return;
	}
	QTimer::singleShot(500, [from, to, attempt]() { renameWithRetry(from, to, attempt + 1); });
}

/* Moves the recording to the Recycle Bin, retrying while the file is locked. */
void trashWithRetry(const QString &path, int attempt)
{
	if (!QFileInfo::exists(path) || QFile::moveToTrash(path)) {
		obs_log(LOG_INFO, "Moved recording to the Recycle Bin: '%s'", path.toUtf8().constData());
		return;
	}
	if (attempt >= 40) {
		obs_log(LOG_WARNING, "Could not delete '%s'", path.toUtf8().constData());
		QMessageBox::warning(mainWindow(), QStringLiteral("Recording Renamer"),
				     QStringLiteral("Could not move\n%1\nto the Recycle Bin.\n\nThe file may be in use.")
					     .arg(QFileInfo(path).fileName()));
		return;
	}
	QTimer::singleShot(500, [path, attempt]() { trashWithRetry(path, attempt + 1); });
}

/* ------------------------------------------------------------ rename dialog */

void showRenameDialog(const QString &path, bool allowTimeout = true)
{
	const QFileInfo fi(path);
	if (!fi.exists()) {
		obs_log(LOG_WARNING, "Recording not found: '%s'", path.toUtf8().constData());
		return;
	}

	auto *dlg = new QDialog(mainWindow(), Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint |
						      Qt::WindowStaysOnTopHint);
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->setWindowTitle(QStringLiteral("Name this recording"));
	dlg->setMinimumWidth(580);

	auto *title = new QLabel(QStringLiteral("Type a name (it goes in front of the file name):"));
	auto *edit = new QLineEdit;
	auto *preview = new QLabel;
	preview->setWordWrap(true);
	auto *status = new QLabel;

	auto *buttons = new QDialogButtonBox;
	QPushButton *save = buttons->addButton(QStringLiteral("Save"), QDialogButtonBox::AcceptRole);
	buttons->addButton(QStringLiteral("Skip"), QDialogButtonBox::RejectRole);
	save->setDefault(true);

	auto *del = new QPushButton(QStringLiteral("Delete"));
	del->setAutoDefault(false);
	del->setToolTip(QStringLiteral("Move this recording to the Recycle Bin"));

	auto *bottom = new QHBoxLayout;
	bottom->addWidget(del);
	bottom->addStretch(1);
	bottom->addWidget(buttons);

	auto *layout = new QVBoxLayout(dlg);
	layout->addWidget(title);
	layout->addWidget(edit);
	layout->addWidget(preview);
	layout->addWidget(status);
	layout->addSpacing(8);
	layout->addLayout(bottom);

	const bool useTimeout = allowTimeout && g_settings.useTimeout && g_settings.timeout > 0;
	auto remaining = std::make_shared<int>(g_settings.timeout);
	auto *timer = new QTimer(dlg);
	timer->setInterval(1000);

	auto updateStatus = [=]() {
		if (!useTimeout)
			status->setText(QStringLiteral("Enter = save   Esc = keep original name"));
		else if (timer->isActive())
			status->setText(
				QStringLiteral("Closing in %1 s - start typing to keep this open").arg(*remaining));
		else
			status->setText(QStringLiteral("Countdown paused - Enter = save, Esc = skip"));
	};
	auto updatePreview = [=]() {
		const QString name = cleanName(edit->text());
		if (name.isEmpty())
			preview->setText(QStringLiteral("Keeps:  %1").arg(fi.fileName()));
		else
			preview->setText(QStringLiteral("New name:  %1%2").arg(name, fi.fileName()));
	};

	QObject::connect(timer, &QTimer::timeout, dlg, [=]() {
		if (--*remaining <= 0) {
			timer->stop();
			dlg->reject();
		} else {
			updateStatus();
		}
	});
	QObject::connect(edit, &QLineEdit::textChanged, dlg, [=](const QString &text) {
		if (!text.isEmpty() && timer->isActive())
			timer->stop();
		updatePreview();
		updateStatus();
	});
	QObject::connect(del, &QPushButton::clicked, dlg, [=]() {
		timer->stop();
		updateStatus();
		const auto answer = QMessageBox::question(
			dlg, QStringLiteral("Delete recording"),
			QStringLiteral("Move this recording to the Recycle Bin?\n\n%1").arg(fi.fileName()),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (answer != QMessageBox::Yes)
			return;
		trashWithRetry(fi.absoluteFilePath(), 0);
		dlg->reject(); /* reject = no rename */
	});
	QObject::connect(buttons, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
	QObject::connect(dlg, &QDialog::accepted, [=]() {
		const QString name = cleanName(edit->text());
		if (name.isEmpty())
			return;
		renameWithRetry(fi.absoluteFilePath(), uniqueTarget(fi, name), 0);
	});

	if (useTimeout)
		timer->start();
	updatePreview();
	updateStatus();

	dlg->show();
	dlg->raise();
	dlg->activateWindow();
	edit->setFocus();
}

void showDialogForLastRecording(bool allowTimeout)
{
	char *p = obs_frontend_get_last_recording();
	if (!p || !*p) {
		bfree(p);
		QMessageBox::information(mainWindow(), QStringLiteral("Recording Renamer"),
					 QStringLiteral("There is no recording from this OBS session yet."));
		return;
	}
	const QString path = QString::fromUtf8(p);
	bfree(p);
	showRenameDialog(path, allowTimeout);
}

/* ------------------------------------------------------------ settings dialog */

void showSettingsDialog()
{
	QDialog dlg(mainWindow());
	dlg.setWindowTitle(QStringLiteral("Recording Renamer Settings"));

	auto *enabled = new QCheckBox(QStringLiteral("Ask for a name when recording stops"));
	enabled->setChecked(g_settings.enabled);

	auto *useTimeout = new QCheckBox(QStringLiteral("Close automatically if I don't type anything"));
	useTimeout->setChecked(g_settings.useTimeout);

	auto *seconds = new QSpinBox;
	seconds->setRange(3, 3600);
	seconds->setSuffix(QStringLiteral(" s"));
	seconds->setValue(g_settings.timeout);
	seconds->setEnabled(g_settings.useTimeout);
	QObject::connect(useTimeout, &QCheckBox::toggled, seconds, &QWidget::setEnabled);

	auto *test = new QPushButton(QStringLiteral("Test dialog on last recording"));
	QObject::connect(test, &QPushButton::clicked, &dlg, []() { showDialogForLastRecording(false); });

	auto *form = new QFormLayout;
	form->addRow(enabled);
	form->addRow(useTimeout);
	form->addRow(QStringLiteral("Timeout:"), seconds);
	form->addRow(test);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	auto *layout = new QVBoxLayout(&dlg);
	layout->addLayout(form);
	layout->addWidget(buttons);

	if (dlg.exec() == QDialog::Accepted) {
		g_settings.useTimeout = useTimeout->isChecked();
		g_settings.timeout = seconds->value();
		setEnabled(enabled->isChecked()); /* also saves */
	}
}

/* ------------------------------------------------------------ dock */

void createDock()
{
	auto *widget = new QWidget;
	auto *check = new QCheckBox(QStringLiteral("Recording Renamer"), widget);
	check->setChecked(g_settings.enabled);
	check->setToolTip(QStringLiteral("Ask for a name when a recording stops"));
	QObject::connect(check, &QCheckBox::toggled, [](bool on) { setEnabled(on); });
	g_dockCheck = check;

	auto *layout = new QVBoxLayout(widget);
	layout->addWidget(check);
	layout->addStretch(1);

	if (!obs_frontend_add_dock_by_id(DOCK_ID, "Scripts", widget))
		obs_log(LOG_WARNING, "Could not add the Scripts dock");
}

/* ------------------------------------------------------------ OBS events */

void onFrontendEvent(enum obs_frontend_event event, void *)
{
	if (event != OBS_FRONTEND_EVENT_RECORDING_STOPPED || !g_settings.enabled)
		return;

	char *p = obs_frontend_get_last_recording();
	if (!p || !*p) {
		bfree(p);
		obs_log(LOG_WARNING, "Recording stopped, but OBS did not report the file path");
		return;
	}
	const QString path = QString::fromUtf8(p);
	bfree(p);

	/* Let OBS finish its own stop handling first. */
	QTimer::singleShot(300, [path]() { showRenameDialog(path); });
}

} // namespace

bool obs_module_load(void)
{
	loadSettings();

	auto *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction("Recording Renamer Settings"));
	QObject::connect(action, &QAction::triggered, []() { showSettingsDialog(); });

	createDock();
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);

	obs_log(LOG_INFO, "Recording Renamer loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	obs_log(LOG_INFO, "Recording Renamer unloaded");
}
