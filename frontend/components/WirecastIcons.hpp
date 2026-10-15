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

/* Palette sampled from Wirecast */
inline const QColor kIconColor{0xDF, 0xE0, 0xE6};
inline const QColor kIconMuted{0x8A, 0x8C, 0x94};
inline const QColor kAmber{0xDF, 0x9B, 0x08};
inline const QColor kRed{0xE4, 0x35, 0x06};
inline const QColor kGreen{0x3D, 0xD2, 0x8C};
inline const QColor kOnAmber{0x1F, 0x25, 0x2E};

QPixmap pixmap(Glyph glyph, const QColor &color, int size, qreal ratio);

/* Icon with the normal colour for Off and a highlight colour for On (checked) */
QIcon icon(Glyph glyph, int size, const QColor &normal = kIconColor, const QColor &checked = kOnAmber);

} // namespace WirecastIcons
