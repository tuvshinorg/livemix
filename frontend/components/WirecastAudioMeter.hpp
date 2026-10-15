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

/* Wirecast-style audio meter beside a monitor, with its controls:
 *
 *   +6 -  ||        dB scale | stereo bar | fader track with gain triangle
 *    0    ||  <|
 *   ...
 *   [spk][hp]       buttons under the meter
 *
 * Program (Live): meters OBS's final mix (track 1). The triangle is the Live
 * volume: the master output level, and what you hear while monitoring Live
 * (drag, scroll, double-click for 0 dB). The speaker button mutes the live
 * output; the headphone button picks which sources you monitor.
 *
 * Preview: meters the audio sources inside the scene in Preview. The headphone
 * button lets you listen to that shot before it goes live, and the triangle is
 * the Preview volume you hear it at.
 *
 * Right-click either meter for its menu. */

#pragma once

#include <obs.hpp>
#include <obs-frontend-api.h>

#include <QTimer>
#include <QWidget>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <vector>

class QMenu;
class QToolButton;

class WirecastAudioMeter : public QWidget {
	Q_OBJECT

public:
	enum class Mode { Preview, Program };

	/* imageRect returns the monitor picture in global coordinates, so the scale lines up with it */
	WirecastAudioMeter(Mode mode, std::function<QRect()> imageRect, QWidget *parent = nullptr);
	~WirecastAudioMeter() override;

	QSize sizeHint() const override;

signals:
	void openMixerRequested();
	void openAdvancedAudioRequested();

private:
	struct Level {
		std::atomic<float> peak[2] = {-100.0f, -100.0f};
	};

	struct SourceMeter {
		OBSWeakSource source;
		obs_volmeter_t *volmeter = nullptr;
		Level level;
	};

	Mode mode;
	std::function<QRect()> imageRect;

	Level programLevel;
	std::map<obs_source_t *, std::unique_ptr<SourceMeter>> sourceMeters;

	float displayPeak[2] = {-60.0f, -60.0f};
	float holdPeak[2] = {-60.0f, -60.0f};
	qint64 holdTime[2] = {0, 0};

	int scaleTop = 6;
	int scaleBottom = 100;

	/* Live output level (Program) */
	float masterDb = 0.0f;
	/* Preview listening level (Preview) */
	float previewDb = 0.0f;
	bool masterMuted = false;
	bool dragging = false;

	/* Preview listening: sources whose monitoring we switched on */
	bool listeningToPreview = false;
	std::vector<OBSWeakSource> previewMonitored;

	QToolButton *outputButton = nullptr;
	QToolButton *monitorButton = nullptr;

	QTimer redrawTimer;
	QTimer previewRefreshTimer;
	bool programConnected = false;
	bool eventCallbackRegistered = false;

	void connectProgram();
	void disconnectProgram();
	void refreshPreviewSources();
	void clearPreviewSources();
	void tick();
	void updateScale();

	int yForDb(float db) const;
	float dbForY(int y) const;

	void loadMasterSettings();
	void saveMasterSettings();
	void applyMasterVolume();
	void setMasterDb(float db);
	void setMasterMuted(bool muted);
	void setPreviewDb(float db);
	/* The triangle's level: Live volume on Program, Preview volume on Preview */
	float faderDb() const;
	void setFaderDb(float db);

	std::vector<OBSSource> monitorableSources() const;
	void setListenToPreview(bool listen);
	void syncPreviewMonitoring();
	void stopPreviewMonitoring();

	void updateButtons();
	void showMonitorMenu(const QPoint &globalPos);
	void addMonitoringItems(QMenu *menu);
	void showContextMenu(const QPoint &globalPos);

	static void onProgramAudio(void *param, size_t mix_idx, struct audio_data *data);
	static void onVolmeter(void *param, const float magnitude[MAX_AUDIO_CHANNELS],
			       const float peak[MAX_AUDIO_CHANNELS], const float inputPeak[MAX_AUDIO_CHANNELS]);
	static void onFrontendEvent(enum obs_frontend_event event, void *data);

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;
	void contextMenuEvent(QContextMenuEvent *event) override;
};
