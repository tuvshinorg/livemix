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

#include "WirecastAudioMeter.hpp"
#include "WirecastIcons.hpp"

#include <OBSApp.hpp>

#include <qt-wrappers.hpp>

#include <QContextMenuEvent>
#include <QDateTime>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <set>

#include "moc_WirecastAudioMeter.cpp"

using WirecastIcons::Glyph;

namespace {
constexpr float kTopDb = 6.0f;
constexpr float kBottomDb = -60.0f;
constexpr float kSilenceDb = -100.0f;
constexpr float kFallDbPerSecond = 24.0f;
constexpr qint64 kHoldMs = 1500;

constexpr const char *kConfigSection = "WirecastAudio";

/* Geometry, sampled from Wirecast's meters */
constexpr int kMeterWidth = 62;
constexpr int kLabelRight = 24;
constexpr int kBarLeft = 28;
constexpr int kChannelWidth = 3;
constexpr int kChannelGap = 1;
constexpr int kMarkerTip = 41;
constexpr int kMarkerBase = 52;
constexpr int kTrackLeft = 53;
constexpr int kTrackWidth = 2;
/* +6 dB to -60 dB, as long as Wirecast's meters */
constexpr int kScaleLength = 390;
constexpr int kButtonSize = 28;
constexpr int kButtonHeight = 22;
constexpr int kButtonIconSize = 18;

const QColor kLabelColor(0x5F, 0x62, 0x6B);
const QColor kTrackColor(0x15, 0x17, 0x1B);
const QColor kMarkerColor(0xD9, 0xD9, 0xD9);
const QColor kMarkerMuted(0x6E, 0x71, 0x7A);

struct Zone {
	float top;
	float bottom;
	QColor unlit;
	QColor lit;
};

const Zone kZones[] = {
	{6.0f, 0.0f, QColor(0x2B, 0x12, 0x0D), QColor(0xE4, 0x35, 0x06)},
	{0.0f, -6.0f, QColor(0x2A, 0x1F, 0x08), QColor(0xDF, 0x9B, 0x08)},
	{-6.0f, -60.0f, QColor(0x0D, 0x1B, 0x16), QColor(0x3D, 0xD2, 0x8C)},
};

/* Shared by both meters: the gain of everything played through the
 * monitoring device. Wirecast monitors Preview or Live, never both, so the
 * Preview volume applies while listening to Preview and the Live volume
 * otherwise. */
float liveMonitorGain = 1.0f;
float previewMonitorGain = 1.0f;
bool previewListening = false;

void applyMonitoringVolume()
{
	obs_set_audio_monitoring_volume(previewListening ? previewMonitorGain : liveMonitorGain);
}

float dbToGain(float db)
{
	return db <= kBottomDb ? 0.0f : std::pow(10.0f, db / 20.0f);
}

/* Snap to unity near 0 dB, as a fader detent */
float snapDb(float db)
{
	return std::clamp(std::fabs(db) < 0.4f ? 0.0f : db, kBottomDb, kTopDb);
}

float toDb(float mul)
{
	return mul > 0.0f ? 20.0f * std::log10(mul) : kSilenceDb;
}

/* Keep the loudest value seen since the meter last read it */
void storeMax(std::atomic<float> &slot, float value)
{
	float current = slot.load(std::memory_order_relaxed);
	while (value > current && !slot.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
	}
}

void collectAudioSources(obs_scene_t *scene, std::set<obs_source_t *> &out, int depth)
{
	if (!scene || depth > 8) {
		return;
	}

	struct Ctx {
		std::set<obs_source_t *> *out;
		std::vector<obs_scene_t *> nested;
	} ctx{&out, {}};

	auto collect = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		Ctx *ctx = static_cast<Ctx *>(param);
		if (!obs_sceneitem_visible(item)) {
			return true;
		}

		obs_source_t *source = obs_sceneitem_get_source(item);
		if (obs_scene_t *child = obs_scene_from_source(source)) {
			ctx->nested.push_back(child);
		} else if (obs_scene_t *group = obs_group_from_source(source)) {
			ctx->nested.push_back(group);
		} else if (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) {
			ctx->out->insert(source);
		}
		return true;
	};
	obs_scene_enum_items(scene, collect, &ctx);

	for (obs_scene_t *child : ctx.nested) {
		collectAudioSources(child, out, depth + 1);
	}
}

QString formatDb(float db)
{
	if (db <= kBottomDb) {
		return QString::fromUtf8("-∞ dB");
	}
	return QString("%1%2 dB").arg(db > 0.0f ? QStringLiteral("+") : QString()).arg(double(db), 0, 'f', 1);
}
} // namespace

WirecastAudioMeter::WirecastAudioMeter(Mode mode_, std::function<QRect()> imageRect_, QWidget *parent)
	: QWidget(parent),
	  mode(mode_),
	  imageRect(std::move(imageRect_))
{
	setObjectName(mode == Mode::Program ? "wirecastProgramMeter" : "wirecastPreviewMeter");
	setFixedWidth(kMeterWidth);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
	setMouseTracking(true);

	monitorButton = new QToolButton(this);
	monitorButton->setIconSize(QSize(kButtonIconSize, kButtonIconSize));
	monitorButton->setFixedSize(kButtonSize, kButtonHeight);
	setClasses(monitorButton, "wc-meter-button");

	if (mode == Mode::Program) {
		outputButton = new QToolButton(this);
		outputButton->setIconSize(QSize(kButtonIconSize, kButtonIconSize));
		outputButton->setFixedSize(kButtonSize, kButtonHeight);
		outputButton->setCheckable(true);
		setClasses(outputButton, "wc-meter-button");
		connect(outputButton, &QToolButton::clicked, this, [this](bool checked) { setMasterMuted(!checked); });

		connect(monitorButton, &QToolButton::clicked, this, [this]() {
			showMonitorMenu(monitorButton->mapToGlobal(QPoint(0, monitorButton->height())));
		});
	} else {
		monitorButton->setCheckable(true);
		connect(monitorButton, &QToolButton::clicked, this, &WirecastAudioMeter::setListenToPreview);
	}

	updateButtons();

	redrawTimer.setInterval(33);
	connect(&redrawTimer, &QTimer::timeout, this, &WirecastAudioMeter::tick);
	redrawTimer.start();

	if (mode == Mode::Preview) {
		/* Picks up items added to or removed from the preview scene */
		previewRefreshTimer.setInterval(1000);
		connect(&previewRefreshTimer, &QTimer::timeout, this, &WirecastAudioMeter::refreshPreviewSources);
	}

	obs_frontend_add_event_callback(&WirecastAudioMeter::onFrontendEvent, this);
	eventCallbackRegistered = true;
}

WirecastAudioMeter::~WirecastAudioMeter()
{
	disconnectProgram();
	stopPreviewMonitoring();
	clearPreviewSources();

	if (eventCallbackRegistered) {
		obs_frontend_remove_event_callback(&WirecastAudioMeter::onFrontendEvent, this);
	}
}

QSize WirecastAudioMeter::sizeHint() const
{
	return QSize(kMeterWidth, 200);
}

/* ------------------------------------------------------------------------- */
/* Level sources                                                             */

void WirecastAudioMeter::connectProgram()
{
	if (programConnected || !obs_get_audio()) {
		return;
	}

	obs_add_raw_audio_callback(0, nullptr, &WirecastAudioMeter::onProgramAudio, this);
	programConnected = true;
}

void WirecastAudioMeter::disconnectProgram()
{
	if (!programConnected) {
		return;
	}

	obs_remove_raw_audio_callback(0, &WirecastAudioMeter::onProgramAudio, this);
	programConnected = false;
}

void WirecastAudioMeter::onProgramAudio(void *param, size_t, struct audio_data *data)
{
	WirecastAudioMeter *meter = static_cast<WirecastAudioMeter *>(param);

	const size_t channels = audio_output_get_channels(obs_get_audio());
	for (size_t c = 0; c < 2; c++) {
		const size_t plane = std::min(c, channels ? channels - 1 : 0);
		const float *samples = reinterpret_cast<const float *>(data->data[plane]);
		if (!samples) {
			continue;
		}

		float peak = 0.0f;
		for (uint32_t i = 0; i < data->frames; i++) {
			peak = std::max(peak, std::fabs(samples[i]));
		}
		storeMax(meter->programLevel.peak[c], toDb(peak));
	}
}

void WirecastAudioMeter::onVolmeter(void *param, const float[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS],
				    const float[MAX_AUDIO_CHANNELS])
{
	SourceMeter *sourceMeter = static_cast<SourceMeter *>(param);

	/* Mono sources feed both sides of the bar */
	const float right = std::isfinite(peak[1]) ? peak[1] : peak[0];
	storeMax(sourceMeter->level.peak[0], peak[0]);
	storeMax(sourceMeter->level.peak[1], right);
}

void WirecastAudioMeter::refreshPreviewSources()
{
	OBSSourceAutoRelease scene = obs_frontend_preview_program_mode_active()
					     ? obs_frontend_get_current_preview_scene()
					     : obs_frontend_get_current_scene();

	std::set<obs_source_t *> wanted;
	collectAudioSources(obs_scene_from_source(scene), wanted, 0);

	for (auto it = sourceMeters.begin(); it != sourceMeters.end();) {
		SourceMeter *entry = it->second.get();
		const bool expired = obs_weak_source_expired(entry->source);
		if (expired || !wanted.count(it->first)) {
			obs_volmeter_remove_callback(entry->volmeter, &WirecastAudioMeter::onVolmeter, entry);
			obs_volmeter_detach_source(entry->volmeter);
			obs_volmeter_destroy(entry->volmeter);
			it = sourceMeters.erase(it);
		} else {
			++it;
		}
	}

	for (obs_source_t *source : wanted) {
		if (sourceMeters.count(source)) {
			continue;
		}

		auto entry = std::make_unique<SourceMeter>();
		entry->source = OBSGetWeakRef(source);
		entry->volmeter = obs_volmeter_create(OBS_FADER_LOG);
		obs_volmeter_add_callback(entry->volmeter, &WirecastAudioMeter::onVolmeter, entry.get());
		obs_volmeter_attach_source(entry->volmeter, source);
		sourceMeters.emplace(source, std::move(entry));
	}

	if (listeningToPreview) {
		syncPreviewMonitoring();
	}
}

void WirecastAudioMeter::clearPreviewSources()
{
	for (auto &[source, entry] : sourceMeters) {
		obs_volmeter_remove_callback(entry->volmeter, &WirecastAudioMeter::onVolmeter, entry.get());
		obs_volmeter_detach_source(entry->volmeter);
		obs_volmeter_destroy(entry->volmeter);
	}
	sourceMeters.clear();
}

/* ------------------------------------------------------------------------- */
/* Live output level                                                         */

void WirecastAudioMeter::loadMasterSettings()
{
	config_t *config = App()->GetUserConfig();
	config_set_default_double(config, kConfigSection, "MasterDb", 0.0);
	config_set_default_bool(config, kConfigSection, "MasterMuted", false);

	config_set_default_double(config, kConfigSection, "PreviewDb", 0.0);

	if (mode == Mode::Preview) {
		setPreviewDb(float(config_get_double(config, kConfigSection, "PreviewDb")));
		return;
	}

	masterDb = std::clamp(float(config_get_double(config, kConfigSection, "MasterDb")), kBottomDb, kTopDb);
	masterMuted = config_get_bool(config, kConfigSection, "MasterMuted");
	applyMasterVolume();
}

void WirecastAudioMeter::saveMasterSettings()
{
	config_t *config = App()->GetUserConfig();
	if (mode == Mode::Preview) {
		config_set_double(config, kConfigSection, "PreviewDb", double(previewDb));
		return;
	}
	config_set_double(config, kConfigSection, "MasterDb", double(masterDb));
	config_set_bool(config, kConfigSection, "MasterMuted", masterMuted);
}

void WirecastAudioMeter::applyMasterVolume()
{
	const float volume = masterMuted ? 0.0f : dbToGain(masterDb);
	obs_set_master_volume(volume);
	/* What you hear while monitoring Live follows the Live volume too */
	liveMonitorGain = volume;
	applyMonitoringVolume();

	setToolTip(QTStr("Wirecast.Audio.LiveLevel").arg(masterMuted ? QTStr("Wirecast.Audio.Muted") : formatDb(masterDb)));
	updateButtons();
	update();
}

void WirecastAudioMeter::setMasterDb(float db)
{
	masterDb = snapDb(db);
	applyMasterVolume();
}

void WirecastAudioMeter::setPreviewDb(float db)
{
	previewDb = snapDb(db);
	previewMonitorGain = dbToGain(previewDb);
	applyMonitoringVolume();
	setToolTip(QTStr("Wirecast.Audio.PreviewLevel").arg(formatDb(previewDb)));
	update();
}

float WirecastAudioMeter::faderDb() const
{
	return mode == Mode::Program ? masterDb : previewDb;
}

void WirecastAudioMeter::setFaderDb(float db)
{
	if (mode == Mode::Program) {
		/* Moving the Live volume turns a muted Live output back on */
		if (masterMuted) {
			masterMuted = false;
		}
		setMasterDb(db);
	} else {
		setPreviewDb(db);
	}
}

void WirecastAudioMeter::setMasterMuted(bool muted)
{
	masterMuted = muted;
	applyMasterVolume();
	saveMasterSettings();
}

/* ------------------------------------------------------------------------- */
/* Headphone monitoring                                                      */

std::vector<OBSSource> WirecastAudioMeter::monitorableSources() const
{
	std::vector<OBSSource> result;

	if (mode == Mode::Preview) {
		for (auto &[source, entry] : sourceMeters) {
			if (OBSSource strong = OBSGetStrongRef(entry->source)) {
				result.push_back(strong);
			}
		}
		return result;
	}

	/* Live: global audio devices plus everything audible in the program scene */
	std::set<obs_source_t *> found;
	for (uint32_t channel = 1; channel <= 6; channel++) {
		OBSSourceAutoRelease source = obs_get_output_source(channel);
		if (source && (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO)) {
			result.emplace_back(source.Get());
			found.insert(source);
		}
	}

	OBSSourceAutoRelease program = obs_frontend_get_current_scene();
	std::set<obs_source_t *> inScene;
	collectAudioSources(obs_scene_from_source(program), inScene, 0);
	for (obs_source_t *source : inScene) {
		if (!found.count(source)) {
			result.emplace_back(source);
		}
	}

	return result;
}

void WirecastAudioMeter::setListenToPreview(bool listen)
{
	listeningToPreview = listen;
	previewListening = listen;
	applyMonitoringVolume();
	if (listen) {
		syncPreviewMonitoring();
	} else {
		stopPreviewMonitoring();
	}
	updateButtons();
}

void WirecastAudioMeter::syncPreviewMonitoring()
{
	std::set<obs_source_t *> inPreview;
	for (auto &[source, entry] : sourceMeters) {
		inPreview.insert(source);
	}

	/* Stop monitoring what left the preview */
	for (auto it = previewMonitored.begin(); it != previewMonitored.end();) {
		OBSSource source = OBSGetStrongRef(*it);
		if (!source) {
			it = previewMonitored.erase(it);
		} else if (!inPreview.count(source)) {
			obs_source_set_monitoring_enabled(source, false);
			it = previewMonitored.erase(it);
		} else {
			++it;
		}
	}

	/* Start monitoring what joined it, leaving sources the user already monitors alone */
	for (const OBSSource &source : monitorableSources()) {
		if (obs_source_get_monitoring_enabled(source)) {
			continue;
		}
		obs_source_set_monitoring_enabled(source, true);
		previewMonitored.push_back(OBSGetWeakRef(source));
	}
}

void WirecastAudioMeter::stopPreviewMonitoring()
{
	for (const OBSWeakSource &weak : previewMonitored) {
		if (OBSSource source = OBSGetStrongRef(weak)) {
			obs_source_set_monitoring_enabled(source, false);
		}
	}
	previewMonitored.clear();
}

void WirecastAudioMeter::addMonitoringItems(QMenu *menu)
{
	const std::vector<OBSSource> sources = monitorableSources();
	if (sources.empty()) {
		menu->addAction(QTStr("Wirecast.Audio.NoSources"))->setEnabled(false);
		return;
	}

	for (const OBSSource &source : sources) {
		QAction *action = menu->addAction(QT_UTF8(obs_source_get_name(source)));
		action->setCheckable(true);
		action->setChecked(obs_source_get_monitoring_enabled(source));
		connect(action, &QAction::toggled, this, [this, source](bool enabled) {
			obs_source_set_monitoring_enabled(source, enabled);
			updateButtons();
		});
	}
}

void WirecastAudioMeter::showMonitorMenu(const QPoint &globalPos)
{
	QMenu menu(this);
	menu.addSection(QTStr("Wirecast.Audio.MonitorHeading"));
	addMonitoringItems(&menu);
	menu.exec(globalPos);
	updateButtons();
}

void WirecastAudioMeter::updateButtons()
{
	using WirecastIcons::kAmber;
	using WirecastIcons::kIconColor;
	using WirecastIcons::kIconMuted;

	if (outputButton) {
		outputButton->setChecked(!masterMuted);
		outputButton->setIcon(WirecastIcons::icon(masterMuted ? Glyph::OutputMuted : Glyph::OutputOn, kButtonIconSize,
							  kIconMuted, kAmber));
		outputButton->setToolTip(masterMuted ? QTStr("Wirecast.Audio.UnmuteLive")
						     : QTStr("Wirecast.Audio.MuteLive"));
	}

	if (mode == Mode::Program) {
		/* Sources only exist once the scene collection has loaded */
		bool anyMonitored = false;
		if (programConnected) {
			for (const OBSSource &source : monitorableSources()) {
				anyMonitored = anyMonitored || obs_source_get_monitoring_enabled(source);
			}
		}
		monitorButton->setIcon(WirecastIcons::icon(anyMonitored ? Glyph::Headphones : Glyph::HeadphonesOff, kButtonIconSize,
							   anyMonitored ? kIconColor : kIconMuted));
		monitorButton->setToolTip(QTStr("Wirecast.Audio.MonitorHeading"));
	} else {
		monitorButton->setChecked(listeningToPreview);
		/* Wirecast's Preview Monitor Audio button is a speaker, amber while you listen */
		monitorButton->setIcon(WirecastIcons::icon(Glyph::OutputOn, kButtonIconSize, kIconMuted, kAmber));
		monitorButton->setToolTip(listeningToPreview ? QTStr("Wirecast.Audio.StopListenPreview")
							     : QTStr("Wirecast.Audio.ListenPreview"));
	}
}

/* ------------------------------------------------------------------------- */
/* Drawing                                                                   */

void WirecastAudioMeter::updateScale()
{
	/* Fixed length, starting level with the top of the monitor picture: the meter
	 * keeps its size when the monitors are made bigger or smaller, and only
	 * shrinks if the window is too short to fit it */
	const int buttonSpace = kButtonHeight + 12;
	int top = 6;
	if (imageRect) {
		const QRect image = imageRect();
		if (image.isValid()) {
			top = std::clamp(mapFromGlobal(image.topLeft()).y(), 6, std::max(6, height() - buttonSpace - 40));
		}
	}
	const int bottom = std::clamp(top + kScaleLength, top + 40, std::max(top + 40, height() - buttonSpace));
	scaleTop = top;
	scaleBottom = bottom;

	/* Buttons sit under the meter, as in Wirecast */
	const int buttonY = scaleBottom + 8;
	if (outputButton) {
		outputButton->move(2, buttonY);
		monitorButton->move(2 + kButtonSize + 4, buttonY);
	} else {
		monitorButton->move(kMeterWidth - kButtonSize - 2, buttonY);
	}
}

int WirecastAudioMeter::yForDb(float db) const
{
	return scaleTop + int(std::round((kTopDb - db) / (kTopDb - kBottomDb) * float(scaleBottom - scaleTop)));
}

float WirecastAudioMeter::dbForY(int y) const
{
	const float t = float(y - scaleTop) / float(std::max(1, scaleBottom - scaleTop));
	return kTopDb - t * (kTopDb - kBottomDb);
}

void WirecastAudioMeter::tick()
{
	float current[2] = {kSilenceDb, kSilenceDb};

	if (mode == Mode::Program) {
		for (int c = 0; c < 2; c++) {
			current[c] = programLevel.peak[c].exchange(kSilenceDb);
		}
	} else {
		for (auto &[source, entry] : sourceMeters) {
			for (int c = 0; c < 2; c++) {
				current[c] = std::max(current[c], entry->level.peak[c].exchange(kSilenceDb));
			}
		}
	}

	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	const float fall = kFallDbPerSecond * float(redrawTimer.interval()) / 1000.0f;

	for (int c = 0; c < 2; c++) {
		const float level = std::clamp(current[c], kBottomDb, kTopDb);
		displayPeak[c] = std::max(level, displayPeak[c] - fall);

		if (level >= holdPeak[c]) {
			holdPeak[c] = level;
			holdTime[c] = now;
		} else if (now - holdTime[c] > kHoldMs && holdPeak[c] > kBottomDb) {
			holdPeak[c] = std::max(kBottomDb, holdPeak[c] - fall);
		}
	}

	/* Repaint every frame: the monitor picture can move without this widget resizing */
	updateScale();
	update();
}

void WirecastAudioMeter::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, false);

	/* dB scale: numbers every 6 dB, dashes in between */
	QFont font = p.font();
	font.setPointSizeF(7.0);
	p.setFont(font);
	p.setPen(kLabelColor);
	/* A shorter meter (smaller window) shows fewer numbers so they never overlap */
	const int textHeight = p.fontMetrics().height();
	const float pxPer3Db = float(scaleBottom - scaleTop) * 3.0f / (kTopDb - kBottomDb);
	int labelStep = 6;
	for (int step : {6, 12, 18, 30, 60}) {
		labelStep = step;
		if (pxPer3Db * float(step / 3) >= float(textHeight)) {
			break;
		}
	}
	const bool dashes = pxPer3Db >= 5.0f;
	for (int db = int(kTopDb); db >= int(kBottomDb); db -= 3) {
		const bool label = db % labelStep == 0 && (db <= 0 || labelStep == 6);
		if (!label && !dashes) {
			continue;
		}
		const int y = yForDb(float(db));
		const QString text = !label  ? QStringLiteral("-")
				     : db > 0 ? QStringLiteral("+%1").arg(db)
					      : QString::number(db);
		p.drawText(QRect(0, y - textHeight / 2, kLabelRight, textHeight), Qt::AlignRight | Qt::AlignVCenter,
			   text);
	}

	/* Stereo bar: dim zones with the live level lit from the bottom */
	for (int c = 0; c < 2; c++) {
		const int x = kBarLeft + c * (kChannelWidth + kChannelGap);
		const int levelY = yForDb(displayPeak[c]);
		const int holdY = yForDb(holdPeak[c]);

		for (const Zone &zone : kZones) {
			const int zoneTop = yForDb(zone.top);
			const int zoneBottom = yForDb(zone.bottom);
			p.fillRect(QRect(x, zoneTop, kChannelWidth, zoneBottom - zoneTop), zone.unlit);

			const int litTop = std::max(zoneTop, levelY);
			if (displayPeak[c] > kBottomDb && litTop < zoneBottom) {
				p.fillRect(QRect(x, litTop, kChannelWidth, zoneBottom - litTop), zone.lit);
			}

			if (holdPeak[c] > kBottomDb && holdY >= zoneTop && holdY < zoneBottom) {
				p.fillRect(QRect(x, holdY, kChannelWidth, 1), zone.lit);
			}
		}
	}

	/* Fader track with the gain triangle */
	p.fillRect(QRect(kTrackLeft, scaleTop, kTrackWidth, scaleBottom - scaleTop), kTrackColor);

	p.setRenderHint(QPainter::Antialiasing, true);
	const float markerDb = faderDb();
	const int markerY = yForDb(markerDb);
	QPainterPath marker;
	marker.moveTo(kMarkerTip, markerY);
	marker.lineTo(kMarkerBase, markerY - 7);
	marker.lineTo(kMarkerBase, markerY + 7);
	marker.closeSubpath();
	p.fillPath(marker, (mode == Mode::Program && masterMuted) ? kMarkerMuted : kMarkerColor);
}

/* ------------------------------------------------------------------------- */
/* Interaction                                                               */

void WirecastAudioMeter::mousePressEvent(QMouseEvent *event)
{
	const QPoint pos = event->position().toPoint();
	if (event->button() == Qt::LeftButton && pos.x() >= kBarLeft && pos.y() >= scaleTop - 8 &&
	    pos.y() <= scaleBottom + 8) {
		dragging = true;
		setFaderDb(dbForY(pos.y()));
		event->accept();
		return;
	}
	QWidget::mousePressEvent(event);
}

void WirecastAudioMeter::mouseMoveEvent(QMouseEvent *event)
{
	const QPoint pos = event->position().toPoint();
	if (dragging) {
		setFaderDb(dbForY(pos.y()));
		return;
	}

	const bool overFader = pos.x() >= kBarLeft && pos.y() >= scaleTop - 8 && pos.y() <= scaleBottom + 8;
	setCursor(overFader ? Qt::SizeVerCursor : Qt::ArrowCursor);
}

void WirecastAudioMeter::mouseReleaseEvent(QMouseEvent *event)
{
	if (dragging && event->button() == Qt::LeftButton) {
		dragging = false;
		saveMasterSettings();
		return;
	}
	QWidget::mouseReleaseEvent(event);
}

void WirecastAudioMeter::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton) {
		setFaderDb(0.0f);
		saveMasterSettings();
	}
}

void WirecastAudioMeter::wheelEvent(QWheelEvent *event)
{
	const int steps = event->angleDelta().y() / 120;
	if (steps != 0) {
		setFaderDb(faderDb() + float(steps));
		saveMasterSettings();
	}
	event->accept();
}

void WirecastAudioMeter::contextMenuEvent(QContextMenuEvent *event)
{
	showContextMenu(event->globalPos());
	event->accept();
}

void WirecastAudioMeter::showContextMenu(const QPoint &globalPos)
{
	QMenu menu(this);

	if (mode == Mode::Program) {
		menu.addSection(QTStr("Wirecast.Audio.LiveHeading"));

		QAction *reset = menu.addAction(QTStr("Wirecast.Audio.ResetLevel"));
		connect(reset, &QAction::triggered, this, [this]() {
			setMasterDb(0.0f);
			saveMasterSettings();
		});

		QAction *mute = menu.addAction(QTStr("Wirecast.Audio.MuteLive"));
		mute->setCheckable(true);
		mute->setChecked(masterMuted);
		connect(mute, &QAction::toggled, this, &WirecastAudioMeter::setMasterMuted);
	} else {
		menu.addSection(QTStr("Wirecast.Audio.PreviewHeading"));

		QAction *reset = menu.addAction(QTStr("Wirecast.Audio.ResetLevel"));
		connect(reset, &QAction::triggered, this, [this]() {
			setPreviewDb(0.0f);
			saveMasterSettings();
		});

		QAction *listen = menu.addAction(QTStr("Wirecast.Audio.ListenPreview"));
		listen->setCheckable(true);
		listen->setChecked(listeningToPreview);
		connect(listen, &QAction::toggled, this, &WirecastAudioMeter::setListenToPreview);
	}

	QMenu *monitorMenu = menu.addMenu(QTStr("Wirecast.Audio.MonitorHeading"));
	addMonitoringItems(monitorMenu);

	menu.addSeparator();
	connect(menu.addAction(QTStr("Wirecast.Audio.OpenMixer")), &QAction::triggered, this,
		&WirecastAudioMeter::openMixerRequested);
	connect(menu.addAction(QTStr("Basic.AdvAudio")), &QAction::triggered, this,
		&WirecastAudioMeter::openAdvancedAudioRequested);

	menu.exec(globalPos);
	updateButtons();
}

/* ------------------------------------------------------------------------- */
/* Frontend events                                                           */

void WirecastAudioMeter::onFrontendEvent(enum obs_frontend_event event, void *data)
{
	WirecastAudioMeter *meter = static_cast<WirecastAudioMeter *>(data);

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		meter->loadMasterSettings();
		if (meter->mode == Mode::Program) {
			meter->connectProgram();
		} else {
			meter->refreshPreviewSources();
			meter->previewRefreshTimer.start();
		}
		meter->updateButtons();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		if (meter->mode == Mode::Preview) {
			meter->refreshPreviewSources();
		}
		meter->updateButtons();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		/* Don't let temporary preview listening get saved into the collection */
		meter->stopPreviewMonitoring();
		meter->clearPreviewSources();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		meter->redrawTimer.stop();
		meter->previewRefreshTimer.stop();
		meter->stopPreviewMonitoring();
		if (meter->mode == Mode::Preview) {
			previewListening = false;
		}
		meter->disconnectProgram();
		meter->clearPreviewSources();
		obs_frontend_remove_event_callback(&WirecastAudioMeter::onFrontendEvent, meter);
		meter->eventCallbackRegistered = false;
		break;
	default:
		break;
	}
}
