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

/* Line icons drawn in code in the style of Wirecast's toolbar glyphs, so the
 * Wirecast layout does not depend on theme SVGs. */

#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>

namespace WirecastIcons {

enum class Glyph {
	Layers,
	Sliders,
	Speaker,
	Clapper,
	Transition,
	More,
	Collapse,
	Expand,
	Broadcast,
	Record,
	Iso,
	Display,
	Go,
	Snapshot,
	OutputOn,
	OutputMuted,
	Headphones,
	HeadphonesOff,
	LayoutDual,
	LayoutPreview,
	LayoutLive,
	Eye,
	Plus,
	Minus,
	Up,
	Down,
	Gear,
	Reset,
};

/* LiveMix "Tally" palette: warm neutrals, with colour only for on-air state
 * (green = Preview, red = Live) */
inline const QColor kIconColor{0xD8, 0xD5, 0xCE};
inline const QColor kIconMuted{0x8E, 0x8B, 0x84};
inline const QColor kIconActive{0xF3, 0xF1, 0xEC};
inline const QColor kRed{0xE5, 0x53, 0x3D};
inline const QColor kGreen{0x4C, 0xC3, 0x8A};
/* Text and icons on the ivory Go button */
inline const QColor kOnIvory{0x17, 0x18, 0x1A};

QPixmap pixmap(Glyph glyph, const QColor &color, int size, qreal ratio);

/* Icon with the normal colour for Off and a highlight colour for On (checked) */
QIcon icon(Glyph glyph, int size, const QColor &normal = kIconMuted, const QColor &checked = kIconActive);

} // namespace WirecastIcons
