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

#include "TransitionBar.hpp"
#include "WirecastIcons.hpp"

#include <OBSApp.hpp>

#include <qt-wrappers.hpp>

#include <QHBoxLayout>
#include <QMenu>
#include <QToolButton>

#include <cstring>
#include <vector>

#include "moc_TransitionBar.cpp"

namespace {
constexpr const char *kConfigSection = "WirecastTransitions";
constexpr const char *kDefaultSlotIds[] = {"cut_transition", "fade_transition"};

std::vector<OBSSource> getTransitions()
{
	std::vector<OBSSource> result;

	obs_frontend_source_list list = {};
	obs_frontend_get_transitions(&list);
	for (size_t i = 0; i < list.sources.num; i++) {
		result.emplace_back(list.sources.array[i]);
	}
	obs_frontend_source_list_free(&list);

	return result;
}

OBSSource findTransition(const std::string &name)
{
	for (const OBSSource &transition : getTransitions()) {
		if (name == obs_source_get_name(transition)) {
			return transition;
		}
	}
	return nullptr;
}

OBSSource findTransitionById(const char *id)
{
	for (const OBSSource &transition : getTransitions()) {
		if (strcmp(obs_source_get_id(transition), id) == 0) {
			return transition;
		}
	}
	return nullptr;
}

/* Wirecast calls a cross-fade "Smooth"; use that for OBS's untouched default Fade */
QString slotLabel(obs_source_t *transition)
{
	if (!transition) {
		return QStringLiteral("-");
	}

	const char *id = obs_source_get_id(transition);
	const char *name = obs_source_get_name(transition);
	if (strcmp(id, "fade_transition") == 0 && strcmp(name, obs_source_get_display_name(id)) == 0) {
		return QTStr("Wirecast.Transition.Smooth");
	}
	return QT_UTF8(name);
}
} // namespace

TransitionBar::TransitionBar(QWidget *parent) : QFrame(parent)
{
	setObjectName("wirecastTransitionBar");

	QHBoxLayout *layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 6, 0, 8);
	layout->setSpacing(10);
	layout->addStretch();

	for (int slot = 0; slot < kSlotCount; slot++) {
		QToolButton *button = new QToolButton(this);
		button->setPopupMode(QToolButton::MenuButtonPopup);
		button->setToolButtonStyle(Qt::ToolButtonTextOnly);
		button->setFixedSize(130, 30);
		button->setMenu(new QMenu(button));
		setClasses(button, "wc-transition-slot");

		connect(button, &QToolButton::clicked, this, [this, slot]() { activateSlot(slot); });
		connect(button->menu(), &QMenu::aboutToShow, this, [this, slot]() { showSlotMenu(slot); });

		slotButtons[slot] = button;
		layout->addWidget(button);
	}

	goButton = new QToolButton(this);
	goButton->setObjectName("wirecastGoButton");
	goButton->setIcon(QIcon(WirecastIcons::pixmap(WirecastIcons::Glyph::Go, WirecastIcons::kIconColor, 22,
						      devicePixelRatioF())));
	goButton->setIconSize(QSize(22, 22));
	goButton->setText(QString::fromUtf8("○"));
	goButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	goButton->setFixedSize(65, 30);
	goButton->setToolTip(QTStr("Wirecast.Go"));
	connect(goButton, &QToolButton::clicked, this, []() { obs_frontend_preview_program_trigger_transition(); });
	layout->addWidget(goButton);

	layout->addStretch();

	obs_frontend_add_event_callback(&TransitionBar::onFrontendEvent, this);
	eventCallbackRegistered = true;
}

TransitionBar::~TransitionBar()
{
	if (eventCallbackRegistered) {
		obs_frontend_remove_event_callback(&TransitionBar::onFrontendEvent, this);
	}
}

void TransitionBar::loadSlots()
{
	config_t *config = App()->GetUserConfig();

	for (int slot = 0; slot < kSlotCount; slot++) {
		std::string key = "Slot" + std::to_string(slot);
		const char *saved = config_get_string(config, kConfigSection, key.c_str());

		OBSSource transition = saved ? findTransition(saved) : nullptr;
		if (!transition) {
			transition = findTransitionById(kDefaultSlotIds[slot]);
		}
		slotNames[slot] = transition ? obs_source_get_name(transition) : "";
	}

	/* Whichever slot holds the current transition is the selected one */
	OBSSourceAutoRelease current = obs_frontend_get_current_transition();
	const char *currentName = current ? obs_source_get_name(current) : "";
	for (int slot = 0; slot < kSlotCount; slot++) {
		if (slotNames[slot] == currentName) {
			activeSlot = slot;
		}
	}
}

void TransitionBar::saveSlots()
{
	config_t *config = App()->GetUserConfig();

	for (int slot = 0; slot < kSlotCount; slot++) {
		std::string key = "Slot" + std::to_string(slot);
		config_set_string(config, kConfigSection, key.c_str(), slotNames[slot].c_str());
	}
}

void TransitionBar::refresh()
{
	for (int slot = 0; slot < kSlotCount; slot++) {
		OBSSource transition = findTransition(slotNames[slot]);
		slotButtons[slot]->setText(slotLabel(transition));
		setClasses(slotButtons[slot],
			   slot == activeSlot ? "wc-transition-slot wc-slot-active" : "wc-transition-slot");
	}

	goButton->setEnabled(obs_frontend_preview_program_mode_active() && !autoLive);
	goButton->setToolTip(autoLive ? QTStr("Wirecast.Go.AutoLive") : QTStr("Wirecast.Go"));
}

QString TransitionBar::slotText(int slot) const
{
	if (slot < 0 || slot >= kSlotCount) {
		return QString();
	}
	return slotLabel(findTransition(slotNames[slot]));
}

void TransitionBar::selectSlot(int slot)
{
	if (slot >= 0 && slot < kSlotCount) {
		activateSlot(slot);
	}
}

void TransitionBar::setAutoLive(bool enabled)
{
	autoLive = enabled;
	refresh();
}

void TransitionBar::activateSlot(int slot)
{
	activeSlot = slot;

	OBSSource transition = findTransition(slotNames[slot]);
	if (transition) {
		obs_frontend_set_current_transition(transition);
	}

	refresh();
}

void TransitionBar::assignTransition(int slot, obs_source_t *transition)
{
	slotNames[slot] = obs_source_get_name(transition);
	saveSlots();
	activateSlot(slot);
}

void TransitionBar::showSlotMenu(int slot)
{
	QMenu *menu = slotButtons[slot]->menu();
	menu->clear();

	for (const OBSSource &transition : getTransitions()) {
		QAction *action = menu->addAction(QT_UTF8(obs_source_get_name(transition)));
		action->setCheckable(true);
		action->setChecked(slotNames[slot] == obs_source_get_name(transition));
		connect(action, &QAction::triggered, this,
			[this, slot, transition]() { assignTransition(slot, transition); });
	}
}

void TransitionBar::onFrontendEvent(enum obs_frontend_event event, void *data)
{
	TransitionBar *bar = static_cast<TransitionBar *>(data);

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_TRANSITION_LIST_CHANGED:
		bar->loadSlots();
		bar->refresh();
		break;
	case OBS_FRONTEND_EVENT_TRANSITION_CHANGED: {
		/* A transition picked elsewhere (e.g. Scene Transitions) lands in the selected slot */
		OBSSourceAutoRelease current = obs_frontend_get_current_transition();
		if (current) {
			std::string name = obs_source_get_name(current);
			bool inSlot = false;
			for (int slot = 0; slot < kSlotCount; slot++) {
				if (bar->slotNames[slot] == name) {
					bar->activeSlot = slot;
					inSlot = true;
				}
			}
			if (!inSlot) {
				bar->slotNames[bar->activeSlot] = name;
				bar->saveSlots();
			}
		}
		bar->refresh();
		break;
	}
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		bar->refresh();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		obs_frontend_remove_event_callback(&TransitionBar::onFrontendEvent, bar);
		bar->eventCallbackRegistered = false;
		break;
	default:
		break;
	}
}
