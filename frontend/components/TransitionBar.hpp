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

/* Transition strip under the monitors:  [Cut v] [Smooth v] [Go ->]  [ ] Auto live
 *
 * Each slot holds one OBS transition (chosen from its drop-down). Clicking a
 * slot makes its transition current; Go takes Preview live with it. */

#pragma once

#include <obs.hpp>
#include <obs-frontend-api.h>

#include <QFrame>

#include <array>
#include <string>

class QCheckBox;
class QToolButton;

class TransitionBar : public QFrame {
	Q_OBJECT

	static constexpr int kSlotCount = 2;

	std::array<QToolButton *, kSlotCount> slotButtons{};
	std::array<std::string, kSlotCount> slotNames;
	int activeSlot = 1;

	QToolButton *goButton = nullptr;
	QCheckBox *autoLiveBox = nullptr;
	bool eventCallbackRegistered = false;

	void loadSlots();
	void saveSlots();
	void refresh();
	void activateSlot(int slot);
	void assignTransition(int slot, obs_source_t *transition);
	void showSlotMenu(int slot);

	static void onFrontendEvent(enum obs_frontend_event event, void *data);

	bool autoLive = false;

public:
	explicit TransitionBar(QWidget *parent = nullptr);
	~TransitionBar() override;

	/* For the Switch menu */
	int slotCount() const { return kSlotCount; }
	QString slotText(int slot) const;
	int currentSlot() const { return activeSlot; }
	void selectSlot(int slot);

	/* With AutoLive on, a shot click goes live by itself and Go is disabled, as in Wirecast */
	void setAutoLive(bool enabled);

signals:
	/* The Auto live checkbox was clicked */
	void autoLiveToggled(bool enabled);
};
