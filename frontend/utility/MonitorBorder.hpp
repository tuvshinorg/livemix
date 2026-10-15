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

#include <obs.h>
#include <graphics/vec4.h>

#include <algorithm>
#include <cmath>

/* Wirecast outlines its monitors: green for Preview, red for Live */
constexpr uint32_t kMonitorBorderPreview = 0x3DB28B;
constexpr uint32_t kMonitorBorderLive = 0xE43506;

/* Draws a frame just outside a cx x cy picture whose top-left is the current origin.
 * Must be called in the graphics thread with a pixel-space ortho projection. */
static inline void DrawMonitorBorder(float cx, float cy, float thickness, uint32_t rgb)
{
	if (cx <= 0.0f || cy <= 0.0f) {
		return;
	}

	const float t = std::max(1.0f, std::round(thickness));

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");

	vec4 colorVal;
	vec4_set(&colorVal, float((rgb >> 16) & 0xFF) / 255.0f, float((rgb >> 8) & 0xFF) / 255.0f,
		 float(rgb & 0xFF) / 255.0f, 1.0f);
	gs_effect_set_vec4(color, &colorVal);

	auto drawRect = [](float x, float y, float w, float h) {
		gs_matrix_push();
		gs_matrix_identity();
		gs_matrix_translate3f(x, y, 0.0f);
		gs_draw_sprite(nullptr, 0, uint32_t(w), uint32_t(h));
		gs_matrix_pop();
	};

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);

	drawRect(-t, -t, cx + 2.0f * t, t);
	drawRect(-t, cy, cx + 2.0f * t, t);
	drawRect(-t, 0.0f, t, cy);
	drawRect(cx, 0.0f, t, cy);

	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}
