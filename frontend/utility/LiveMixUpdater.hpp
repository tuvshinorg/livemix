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

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QThread>

class QProgressDialog;
class QWidget;

/* LiveMix updates come only from the GitHub releases of the LiveMix repository
 * (LIVEMIX_GITHUB_REPO). The newest published release is compared with
 * LIVEMIX_VERSION; when it is newer the user is asked, and on "Update Now" the
 * release's Windows installer is downloaded, verified and run. */

struct LiveMixRelease {
	QString version;
	QString notes;
	QString pageUrl;
	QString assetName;
	QString assetUrl;
	qint64 assetSize = 0;
	QString assetSha256;
};

class LiveMixReleaseCheck : public QThread {
	Q_OBJECT

	void run() override;

signals:
	void checked(bool ok, const LiveMixRelease &release, const QString &error);
};

class LiveMixInstallerDownload : public QThread {
	Q_OBJECT

	LiveMixRelease release;
	QString path;

	void run() override;

public:
	LiveMixInstallerDownload(const LiveMixRelease &release, const QString &path)
		: release(release),
		  path(path)
	{
	}

signals:
	void progress(qint64 received, qint64 total);
	void downloaded(bool ok, const QString &path, const QString &error);
};

class LiveMixUpdater : public QObject {
	Q_OBJECT

	QPointer<QWidget> window;
	QPointer<LiveMixReleaseCheck> checkThread;
	QPointer<LiveMixInstallerDownload> downloadThread;
	QPointer<QProgressDialog> progressDialog;
	bool manual = false;

	void releaseChecked(bool ok, const LiveMixRelease &release, const QString &error);
	void startDownload(const LiveMixRelease &release);
	void installerDownloaded(bool ok, const QString &path, const QString &error);
	bool launchInstaller(const QString &path);
	void finish();

public:
	explicit LiveMixUpdater(QWidget *window);
	~LiveMixUpdater();

	void check(bool manualCheck);
	bool isBusy() const;

	/* Compares dotted versions ("v1.2.3", "1.2"); returns <0, 0 or >0 */
	static int compareVersions(const QString &a, const QString &b);

signals:
	/* The check (and any download) is over and LiveMix keeps running */
	void idle();
};
