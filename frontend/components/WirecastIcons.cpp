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

#include "WirecastIcons.hpp"

#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>

namespace WirecastIcons {

namespace {
/* Thin Lucide-based line icons shipped in data/themes/Wirecast/glyphs. They are
 * drawn in the theme text colour and recoloured here per use. */
constexpr const char *kSvgColor = "#DFE0E6";

const char *glyphFile(Glyph glyph)
{
	switch (glyph) {
	case Glyph::Layers:
		return "layers";
	case Glyph::Sliders:
		return "sliders";
	case Glyph::Speaker:
		return "speaker";
	case Glyph::Clapper:
		return "effects";
	case Glyph::Transition:
		return "transition";
	case Glyph::More:
		return "more";
	case Glyph::Collapse:
		return "collapse";
	case Glyph::Expand:
		return "expand";
	case Glyph::Broadcast:
		return "broadcast";
	case Glyph::Record:
		return "record";
	case Glyph::Iso:
		return "replay";
	case Glyph::Display:
		return "display";
	case Glyph::Go:
		return "go";
	case Glyph::Snapshot:
		return "snapshot";
	case Glyph::OutputOn:
		return "output-on";
	case Glyph::OutputMuted:
		return "output-muted";
	case Glyph::Headphones:
		return "headphones";
	case Glyph::HeadphonesOff:
		return "headphones-off";
	/* ':' marks the general Wirecast theme icons outside glyphs/ */
	case Glyph::Eye:
		return ":visible";
	case Glyph::Plus:
		return ":plus";
	case Glyph::Minus:
		return ":minus";
	case Glyph::Up:
		return ":up";
	case Glyph::Down:
		return ":down";
	case Glyph::Gear:
		return ":cogs";
	case Glyph::Reset:
		return ":revert";
	case Glyph::LayoutDual:
	case Glyph::LayoutPreview:
	case Glyph::LayoutLive:
		break;
	}
	return nullptr;
}

QByteArray loadSvg(const char *name)
{
	if (!name) {
		return QByteArray();
	}

	static QHash<QString, QByteArray> cache;

	const QString key = QString::fromUtf8(name);
	auto it = cache.find(key);
	if (it != cache.end()) {
		return it.value();
	}

	const QString path = key.startsWith(':') ? key.mid(1) : QStringLiteral("glyphs/") + key;
	QFile file(QStringLiteral("theme:Wirecast/%1.svg").arg(path));
	QByteArray data = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
	cache.insert(key, data);
	return data;
}

/* Monitor layout swatches are flat colour blocks, as in Wirecast */
void drawLayoutSwatches(QPainter &p, int size, Glyph glyph)
{
	const qreal unit = size / 24.0;
	p.setPen(Qt::NoPen);
	if (glyph == Glyph::LayoutDual) {
		p.setBrush(kGreen);
		p.drawRect(QRectF(4.0 * unit, 9.0 * unit, 7.0 * unit, 6.0 * unit));
		p.setBrush(kRed);
		p.drawRect(QRectF(13.0 * unit, 9.0 * unit, 7.0 * unit, 6.0 * unit));
	} else {
		p.setBrush(glyph == Glyph::LayoutPreview ? kGreen : kRed);
		p.drawRect(QRectF(8.5 * unit, 9.0 * unit, 7.0 * unit, 6.0 * unit));
	}
}
/* Wirecast's output, transport and audio buttons use solid icons rather than
 * line icons; they are drawn here on a 24-unit grid. Returns false for glyphs
 * that come from SVG files. */
bool drawSolidGlyph(QPainter &p, Glyph glyph, const QColor &color, int size)
{
	p.save();
	p.scale(size / 24.0, size / 24.0);
	p.setBrush(color);
	QPen pen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);

	bool drawn = true;
	switch (glyph) {
	case Glyph::Broadcast: {
		/* Wireless arcs over a small mast */
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		for (qreal r : {3.6, 7.2, 10.8}) {
			p.drawArc(QRectF(12 - r, 15 - r, 2 * r, 2 * r), 45 * 16, 90 * 16);
		}
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		QPolygonF mast;
		mast << QPointF(12, 15.5) << QPointF(9.2, 21) << QPointF(14.8, 21);
		p.drawPolygon(mast);
		break;
	}
	case Glyph::Record:
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(QPointF(12, 12), 9.0, 9.0);
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawEllipse(QPointF(12, 12), 4.6, 4.6);
		break;
	case Glyph::OutputOn:
	case Glyph::OutputMuted: {
		QPolygonF body;
		body << QPointF(2.5, 9) << QPointF(7, 9) << QPointF(12.5, 4) << QPointF(12.5, 20) << QPointF(7, 15)
		     << QPointF(2.5, 15);
		p.setPen(Qt::NoPen);
		p.drawPolygon(body);
		p.setBrush(Qt::NoBrush);
		pen.setWidthF(1.8);
		p.setPen(pen);
		if (glyph == Glyph::OutputOn) {
			p.drawArc(QRectF(12.5 - 4.5, 12 - 4.5, 9, 9), -50 * 16, 100 * 16);
			p.drawArc(QRectF(12.5 - 8, 12 - 8, 16, 16), -50 * 16, 100 * 16);
		} else {
			p.drawLine(QPointF(15.5, 9), QPointF(21.5, 15));
			p.drawLine(QPointF(21.5, 9), QPointF(15.5, 15));
		}
		break;
	}
	case Glyph::Go: {
		/* Thick solid arrow */
		QPolygonF arrow;
		arrow << QPointF(1.5, 9.6) << QPointF(13, 9.6) << QPointF(13, 5.5) << QPointF(22.5, 12)
		      << QPointF(13, 18.5) << QPointF(13, 14.4) << QPointF(1.5, 14.4);
		p.setPen(Qt::NoPen);
		p.drawPolygon(arrow);
		break;
	}
	case Glyph::Snapshot: {
		/* Solid camera with a cut-out lens */
		QPainterPath body;
		body.setFillRule(Qt::OddEvenFill);
		body.addRoundedRect(QRectF(2, 7, 20, 13.5), 2.2, 2.2);
		body.addRoundedRect(QRectF(8, 4, 8, 4.5), 1.2, 1.2);
		body = body.simplified();
		QPainterPath lens;
		lens.addEllipse(QPointF(12, 13.6), 4.6, 4.6);
		body = body.subtracted(lens);
		body.addEllipse(QPointF(12, 13.6), 2.8, 2.8);
		p.setPen(Qt::NoPen);
		p.drawPath(body);
		break;
	}
	default:
		drawn = false;
		break;
	}

	p.restore();
	return drawn;
}
} // namespace

QPixmap pixmap(Glyph glyph, const QColor &color, int size, qreal ratio)
{
	QPixmap result(QSize(size, size) * ratio);
	result.setDevicePixelRatio(ratio);
	result.fill(Qt::transparent);

	QPainter p(&result);
	p.setRenderHint(QPainter::Antialiasing);

	if (glyph == Glyph::LayoutDual || glyph == Glyph::LayoutPreview || glyph == Glyph::LayoutLive) {
		drawLayoutSwatches(p, size, glyph);
		return result;
	}

	if (drawSolidGlyph(p, glyph, color, size)) {
		return result;
	}

	QByteArray svg = loadSvg(glyphFile(glyph));
	if (svg.isEmpty()) {
		return result;
	}
	svg.replace(kSvgColor, color.name(QColor::HexRgb).toUtf8());
	/* Wirecast's sidebar chevrons are bold */
	if (glyph == Glyph::Collapse || glyph == Glyph::Expand) {
		svg.replace("stroke-width=\"1.5\"", "stroke-width=\"3\"");
	}

	QSvgRenderer renderer(svg);
	renderer.render(&p, QRectF(0, 0, size, size));
	return result;
}

QIcon icon(Glyph glyph, int size, const QColor &normal, const QColor &checked)
{
	const qreal ratio = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;

	QIcon result;
	result.addPixmap(pixmap(glyph, normal, size, ratio), QIcon::Normal, QIcon::Off);
	result.addPixmap(pixmap(glyph, checked, size, ratio), QIcon::Normal, QIcon::On);
	result.addPixmap(pixmap(glyph, kIconMuted, size, ratio), QIcon::Disabled, QIcon::Off);
	return result;
}

} // namespace WirecastIcons
