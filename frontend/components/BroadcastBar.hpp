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

/* Wirecast-style output toolbar that sits above the monitors:
 *   [stream] Stream: 00:00:00  [record] Record: 00:00:00  [replay] Replay: 00:00:00  [display]
 * with the monitor layout switch (Preview + Live / Live only) on the right. */

#pragma once

#include <obs-frontend-api.h>

#include <QElapsedTimer>
#include <QFrame>
#include <QTimer>

class QLabel;
class QToolButton;

class BroadcastBar : public QFrame {
	Q_OBJECT

	struct OutputClock {
		QToolButton *button = nullptr;
		QLabel *label = nullptr;
		QString caption;
		int glyph = 0;
		QElapsedTimer elapsed;
		bool active = false;
	};

	OutputClock stream;
	OutputClock record;
	OutputClock replay;

	QToolButton *expandButton = nullptr;
	QToolButton *virtualCamButton = nullptr;
	QToolButton *dualButton = nullptr;
	QToolButton *previewButton = nullptr;
	QToolButton *liveButton = nullptr;

	QTimer clockTimer;
	bool eventCallbackRegistered = false;

	void setupClock(OutputClock &clock, int glyph, const QString &caption, const QString &tooltip);
	void setClockActive(OutputClock &clock, bool active);
	void updateClockLabel(OutputClock &clock);
	void updateClocks();
	void setVirtualCamActive(bool active);

	static void onFrontendEvent(enum obs_frontend_event event, void *data);

public:
	explicit BroadcastBar(QWidget *parent = nullptr);
	~BroadcastBar() override;

	void setSidebarCollapsed(bool collapsed);

	/* Monitor layout shown by the top-right buttons (Layout menu): */
	enum MonitorMode { LiveAndPreview = 0, PreviewOnly = 1, LiveOnly = 2 };
	void setMonitorMode(int mode);

signals:
	void streamClicked();
	void recordClicked();
	void replayClicked();
	void virtualCamClicked();
	void monitorModeRequested(int mode);
	void expandSidebarClicked();
};
