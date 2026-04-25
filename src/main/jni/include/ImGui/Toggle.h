#pragma once
#include "imgui_internal.h"

void Toggle(const char* str_id, bool* v)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    float height = 32.0f;
    float width = height * 1.8f;
    float radius = height * 0.5f;

    ImGui::InvisibleButton(str_id, ImVec2(width, height));
    if (ImGui::IsItemClicked())
        *v = !*v;

    ImGuiContext& g = *GImGui;
    float ANIM_SPEED = 0.15f;
    float t = *v ? 1.0f : 0.0f;

    if (g.LastActiveId == g.CurrentWindow->GetID(str_id))
    {
        float t_anim = ImSaturate(g.LastActiveIdTimer / ANIM_SPEED);
        t = *v ? t_anim : (1.0f - t_anim);
        t = t * t * (3 - 2 * t);
    }

    ImVec4 bg_on = ImVec4(0.3f, 0.9f, 0.4f, 1.0f);
    ImVec4 bg_off = ImVec4(0.6f, 0.1f, 0.1f, 1.0f);
    ImVec4 bg_color = ImLerp(bg_off, bg_on, t);

    ImU32 col_bg = ImGui::GetColorU32(bg_color);
    ImU32 col_glow = ImGui::GetColorU32(ImVec4(bg_color.x, bg_color.y, bg_color.z, 0.3f));

    draw_list->AddRectFilled(ImVec2(p.x - 2, p.y - 2), ImVec2(p.x + width + 2, p.y + height + 2), col_glow, radius + 2.0f);
    draw_list->AddRectFilled(p, ImVec2(p.x + width, p.y + height), col_bg, radius);

    ImVec2 knob_center = ImVec2(p.x + radius + t * (width - 2 * radius), p.y + radius);
    draw_list->AddCircleFilled(ImVec2(knob_center.x, knob_center.y + 2), radius - 3.0f, IM_COL32(0, 0, 0, 50));
    draw_list->AddCircleFilled(knob_center, radius - 2.0f, IM_COL32(255, 255, 255, 255));
    draw_list->AddCircle(knob_center, radius - 2.0f, IM_COL32(220, 220, 220, 100), 32, 1.5f);

    draw_list->AddRect(p, ImVec2(p.x + width, p.y + height), IM_COL32(255, 255, 255, 60), radius, 0, 1.0f);

    ImGui::SameLine();
    ImGui::Text("%s", str_id);
}
