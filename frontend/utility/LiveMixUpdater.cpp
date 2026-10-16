/******************************************************************************
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "LiveMixUpdater.hpp"

#include <OBSApp.hpp>
#include <dialogs/OBSUpdate.hpp>
#include <ui-config.h>

#include <qt-wrappers.hpp>
#include <util/curl/curl-helper.h>

#include <nlohmann/json.hpp>

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QTextDocument>
#include <QUrl>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "moc_LiveMixUpdater.cpp"

#define LIVEMIX_LATEST_RELEASE_URL "https://api.github.com/repos/" LIVEMIX_GITHUB_REPO "/releases/latest"

namespace {

constexpr const char *kSkipVersionKey = "LiveMixSkipVersion";

struct Transfer {
	QThread *thread = nullptr;
	std::string *body = nullptr;
	QFile *file = nullptr;
	QCryptographicHash *hash = nullptr;
	std::function<void(qint64, qint64)> onProgress;
};

size_t WriteData(char *data, size_t size, size_t count, void *param)
{
	Transfer *transfer = static_cast<Transfer *>(param);
	const size_t total = size * count;

	if (transfer->file) {
		if (transfer->file->write(data, qint64(total)) != qint64(total)) {
			return 0;
		}
		if (transfer->hash) {
			transfer->hash->addData(QByteArrayView(data, qsizetype(total)));
		}
	} else {
		transfer->body->append(data, total);
	}
	return total;
}

int TransferProgress(void *param, curl_off_t received_total, curl_off_t received, curl_off_t, curl_off_t)
{
	Transfer *transfer = static_cast<Transfer *>(param);
	if (transfer->thread->isInterruptionRequested()) {
		return 1;
	}
	if (transfer->onProgress) {
		transfer->onProgress(qint64(received), qint64(received_total));
	}
	return 0;
}

/* Performs a GET request; follows redirects, since release assets are served from a CDN */
bool HttpGet(const char *url, Transfer &transfer, const std::vector<std::string> &headers, long &status,
	     QString &error)
{
	char errorBuffer[CURL_ERROR_SIZE] = {};
	CURL *curl = curl_easy_init();
	if (!curl) {
		error = QStringLiteral("curl_easy_init failed");
		return false;
	}

	struct curl_slist *headerList = nullptr;
	headerList = curl_slist_append(headerList, "User-Agent: LiveMix/" LIVEMIX_VERSION);
	for (const std::string &header : headers) {
		headerList = curl_slist_append(headerList, header.c_str());
	}

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, TransferProgress);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
	curl_obs_set_revoke_setting(curl);

	CURLcode code = curl_easy_perform(curl);
	status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

	curl_slist_free_all(headerList);
	curl_easy_cleanup(curl);

	if (code != CURLE_OK) {
		error = errorBuffer[0] ? QString::fromUtf8(errorBuffer) : QString::fromUtf8(curl_easy_strerror(code));
		return false;
	}
	return true;
}

/* GitHub sends null for empty fields, which value() would refuse */
QString JsonString(const nlohmann::json &object, const char *key)
{
	auto it = object.find(key);
	return it != object.end() && it->is_string() ? QString::fromStdString(it->get<std::string>()) : QString();
}

QString StripTagPrefix(QString tag)
{
	tag = tag.trimmed();
	if (tag.startsWith('v', Qt::CaseInsensitive)) {
		tag.remove(0, 1);
	}
	return tag;
}

} // namespace

/* ------------------------------------------------------------------------ */

void LiveMixReleaseCheck::run()
{
	std::string body;
	Transfer transfer;
	transfer.thread = this;
	transfer.body = &body;

	long status = 0;
	QString error;
	const std::vector<std::string> headers = {"Accept: application/vnd.github+json",
						  "X-GitHub-Api-Version: 2022-11-28"};

	if (!HttpGet(LIVEMIX_LATEST_RELEASE_URL, transfer, headers, status, error)) {
		emit checked(false, {}, error);
		return;
	}

	/* No published release yet */
	if (status == 404) {
		emit checked(true, {}, {});
		return;
	}
	if (status != 200) {
		emit checked(false, {}, QStringLiteral("GitHub answered HTTP %1").arg(status));
		return;
	}

	try {
		nlohmann::json json = nlohmann::json::parse(body);

		LiveMixRelease release;
		release.version = StripTagPrefix(JsonString(json, "tag_name"));
		release.notes = JsonString(json, "body");
		release.pageUrl = JsonString(json, "html_url");

		/* Pick the Windows installer, preferring an asset named "...Setup....exe" */
		int bestScore = 0;
		auto assets = json.find("assets");
		if (assets == json.end() || !assets->is_array()) {
			assets = json.end();
		}
		for (const nlohmann::json &asset : assets != json.end() ? *assets : nlohmann::json::array()) {
			QString name = JsonString(asset, "name");
			if (!name.endsWith(".exe", Qt::CaseInsensitive)) {
				continue;
			}

			int score = name.contains("setup", Qt::CaseInsensitive) ? 2 : 1;
			if (score <= bestScore) {
				continue;
			}
			bestScore = score;

			release.assetName = name;
			release.assetUrl = JsonString(asset, "browser_download_url");
			auto size = asset.find("size");
			release.assetSize = size != asset.end() && size->is_number_integer() ? size->get<qint64>() : 0;

			QString digest = JsonString(asset, "digest");
			release.assetSha256 = digest.startsWith("sha256:") ? digest.mid(7).toLower() : QString();
		}

		emit checked(true, release, {});
	} catch (const nlohmann::json::exception &e) {
		emit checked(false, {}, QString::fromUtf8(e.what()));
	}
}

/* ------------------------------------------------------------------------ */

void LiveMixInstallerDownload::run()
{
	const QString partPath = path + ".part";
	QFile file(partPath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		emit downloaded(false, path, file.errorString());
		return;
	}

	QCryptographicHash hash(QCryptographicHash::Sha256);
	Transfer transfer;
	transfer.thread = this;
	transfer.file = &file;
	transfer.hash = &hash;

	qint64 lastReported = -1;
	transfer.onProgress = [&](qint64 received, qint64 total) {
		if (total <= 0) {
			total = release.assetSize;
		}
		/* Report at most every 256 KB to keep the queued signals light */
		if (received == lastReported || (received - lastReported < 256 * 1024 && received != total)) {
			return;
		}
		lastReported = received;
		emit progress(received, total);
	};

	long status = 0;
	QString error;
	const QByteArray url = release.assetUrl.toUtf8();
	bool ok = HttpGet(url.constData(), transfer, {"Accept: application/octet-stream"}, status, error);
	file.close();

	if (isInterruptionRequested()) {
		QFile::remove(partPath);
		emit downloaded(false, path, {});
		return;
	}
	if (ok && status != 200) {
		ok = false;
		error = QStringLiteral("HTTP %1").arg(status);
	}
	if (ok && release.assetSize > 0 && file.size() != release.assetSize) {
		ok = false;
		error = QStringLiteral("expected %1 bytes, received %2").arg(release.assetSize).arg(file.size());
	}
	if (ok && !release.assetSha256.isEmpty() && QString::fromLatin1(hash.result().toHex()) != release.assetSha256) {
		ok = false;
		error = QStringLiteral("the SHA-256 checksum does not match");
	}

	if (ok) {
		QFile::remove(path);
		if (!QFile::rename(partPath, path)) {
			ok = false;
			error = QStringLiteral("could not move the installer to %1").arg(path);
		}
	}
	if (!ok) {
		QFile::remove(partPath);
	}

	emit downloaded(ok, path, error);
}

/* ------------------------------------------------------------------------ */

LiveMixUpdater::LiveMixUpdater(QWidget *window_) : QObject(window_), window(window_) {}

LiveMixUpdater::~LiveMixUpdater()
{
	for (QThread *thread : {static_cast<QThread *>(checkThread.data()), static_cast<QThread *>(downloadThread.data())}) {
		if (thread) {
			thread->requestInterruption();
			thread->wait();
			delete thread;
		}
	}
}

bool LiveMixUpdater::isBusy() const
{
	return checkThread || downloadThread || progressDialog;
}

int LiveMixUpdater::compareVersions(const QString &a, const QString &b)
{
	auto parts = [](const QString &version) {
		/* "1.2.3-beta1" compares as 1.2.3 */
		return StripTagPrefix(version).section('-', 0, 0).split('.');
	};

	const QStringList left = parts(a);
	const QStringList right = parts(b);
	for (qsizetype i = 0; i < std::max(left.size(), right.size()); i++) {
		int l = i < left.size() ? left[i].toInt() : 0;
		int r = i < right.size() ? right[i].toInt() : 0;
		if (l != r) {
			return l < r ? -1 : 1;
		}
	}
	return 0;
}

void LiveMixUpdater::check(bool manualCheck)
{
	if (isBusy()) {
		return;
	}
	manual = manualCheck;

	LiveMixReleaseCheck *thread = new LiveMixReleaseCheck();
	connect(thread, &LiveMixReleaseCheck::checked, this, &LiveMixUpdater::releaseChecked);
	connect(thread, &QThread::finished, thread, &QObject::deleteLater);
	checkThread = thread;
	thread->start();
}

void LiveMixUpdater::releaseChecked(bool ok, const LiveMixRelease &release, const QString &error)
{
	checkThread.clear();

	if (!ok) {
		blog(LOG_WARNING, "LiveMix update check failed: %s", QT_TO_UTF8(error));
		if (manual) {
			OBSMessageBox::warning(window, QTStr("LiveMix.Update.CheckFailed.Title"),
					       QTStr("LiveMix.Update.CheckFailed.Text").arg(error));
		}
		finish();
		return;
	}

	if (release.version.isEmpty() || compareVersions(release.version, LIVEMIX_VERSION) <= 0) {
		blog(LOG_INFO, "LiveMix %s is up to date (latest release: %s)", LIVEMIX_VERSION,
		     release.version.isEmpty() ? "none" : QT_TO_UTF8(release.version));
		if (manual) {
			OBSMessageBox::information(window, QTStr("Updater.NoUpdatesAvailable.Title"),
						   QTStr("LiveMix.Update.UpToDate").arg(LIVEMIX_VERSION));
		}
		finish();
		return;
	}

	const char *skipVersion = config_get_string(App()->GetAppConfig(), "General", kSkipVersionKey);
	if (!manual && skipVersion && release.version == skipVersion) {
		finish();
		return;
	}

	blog(LOG_INFO, "LiveMix %s is available (running %s)", QT_TO_UTF8(release.version), LIVEMIX_VERSION);

	QTextDocument notes;
	notes.setMarkdown(release.notes);
	QString text = QStringLiteral("<h3>%1</h3><p>%2</p>")
			       .arg(QTStr("LiveMix.Update.Heading").arg(release.version).toHtmlEscaped(),
				    QTStr("LiveMix.Update.Current").arg(LIVEMIX_VERSION).toHtmlEscaped());
	if (!release.notes.trimmed().isEmpty()) {
		text += QStringLiteral("<hr>") + notes.toHtml();
	}

	OBSUpdate dialog(window, manual, text);
	int answer = dialog.exec();

	if (answer == OBSUpdate::Skip) {
		config_set_string(App()->GetAppConfig(), "General", kSkipVersionKey, QT_TO_UTF8(release.version));
		config_save_safe(App()->GetAppConfig(), "tmp", nullptr);
		finish();
		return;
	}
	if (answer != OBSUpdate::Yes) {
		finish();
		return;
	}

	/* A release without an installer can only be fetched from its page */
	if (release.assetUrl.isEmpty()) {
		QDesktopServices::openUrl(QUrl(release.pageUrl));
		finish();
		return;
	}

	startDownload(release);
}

void LiveMixUpdater::startDownload(const LiveMixRelease &release)
{
	QString fileName = QFileInfo(release.assetName).fileName();
	if (fileName.isEmpty() || !fileName.endsWith(".exe", Qt::CaseInsensitive)) {
		fileName = QStringLiteral("LiveMix-%1-Setup.exe").arg(release.version);
	}
	const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(fileName);

	QProgressDialog *dialog = new QProgressDialog(QTStr("LiveMix.Update.Downloading").arg(release.version),
						      QTStr("Cancel"), 0, 100, window);
	dialog->setWindowTitle(QTStr("Updater.Title"));
	dialog->setWindowModality(Qt::WindowModal);
	dialog->setMinimumDuration(0);
	dialog->setAutoClose(false);
	dialog->setAutoReset(false);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	progressDialog = dialog;

	LiveMixInstallerDownload *thread = new LiveMixInstallerDownload(release, path);
	connect(thread, &LiveMixInstallerDownload::progress, dialog, [dialog](qint64 received, qint64 total) {
		if (total > 0) {
			dialog->setRange(0, 1000);
			dialog->setValue(int(received * 1000 / total));
		} else {
			dialog->setRange(0, 0);
		}
	});
	connect(thread, &LiveMixInstallerDownload::downloaded, this, &LiveMixUpdater::installerDownloaded);
	connect(thread, &QThread::finished, thread, &QObject::deleteLater);
	connect(dialog, &QProgressDialog::canceled, thread, &QThread::requestInterruption);
	downloadThread = thread;

	dialog->show();
	thread->start();
}

void LiveMixUpdater::installerDownloaded(bool ok, const QString &path, const QString &error)
{
	downloadThread.clear();
	if (progressDialog) {
		progressDialog->close();
		progressDialog.clear();
	}

	if (!ok) {
		/* An empty error means the user cancelled */
		if (!error.isEmpty()) {
			blog(LOG_WARNING, "LiveMix update download failed: %s", QT_TO_UTF8(error));
			OBSMessageBox::warning(window, QTStr("LiveMix.Update.Failed.Title"),
					       QTStr("LiveMix.Update.DownloadFailed").arg(error));
		}
		finish();
		return;
	}

	if (!launchInstaller(path)) {
		finish();
		return;
	}

	/* The installer waits for LiveMix to close, replaces it and starts it again */
	config_set_string(App()->GetAppConfig(), "General", kSkipVersionKey, "");
	config_save_safe(App()->GetAppConfig(), "tmp", nullptr);
	finish();
	if (window) {
		QMetaObject::invokeMethod(window, &QWidget::close, Qt::QueuedConnection);
	}
}

bool LiveMixUpdater::launchInstaller(const QString &path)
{
	const std::wstring file = QDir::toNativeSeparators(path).toStdWString();
	const std::wstring parameters = L"/SILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS";

	SHELLEXECUTEINFOW execInfo = {};
	execInfo.cbSize = sizeof(execInfo);
	execInfo.lpVerb = L"open";
	execInfo.lpFile = file.c_str();
	execInfo.lpParameters = parameters.c_str();
	execInfo.nShow = SW_SHOWNORMAL;

	if (!ShellExecuteExW(&execInfo)) {
		DWORD code = GetLastError();
		blog(LOG_WARNING, "Could not start the LiveMix installer '%s': %lu", QT_TO_UTF8(path), code);
		/* Declining the administrator prompt is not an error worth a dialog */
		if (code != ERROR_CANCELLED) {
			OBSMessageBox::warning(window, QTStr("LiveMix.Update.Failed.Title"),
					       QTStr("LiveMix.Update.LaunchFailed"));
		}
		return false;
	}

	blog(LOG_INFO, "Started the LiveMix installer '%s'", QT_TO_UTF8(path));
	return true;
}

void LiveMixUpdater::finish()
{
	emit idle();
}
