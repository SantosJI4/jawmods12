#include "imgui.h"
#include "imgui_internal.h"
#include "Unity.h"

float getDistance(Vector3 self, Vector3 object) {
	float x, y, z;
	x = self.x - object.x;
	y = self.y - object.y;
	z = self.z - object.z;
    return (float)(sqrt(x * x + y * y + z * z));
}

namespace Draw {
    void Line(ImVec2 start, ImVec2 end, ImVec4 color, float thickness = 3.0f) {
        auto background = ImGui::GetBackgroundDrawList();
        if (background) {
            background->AddLine(start, end, ImColor(color.x, color.y, color.z, color.w), thickness);
        }
    }
    void DrawLineGlow(ImVec2 start, ImVec2 end, ImColor col, float size, int gsize) {
        ImGui::GetBackgroundDrawList()->AddLine(start, end, col, size);
        for (int i = 0; i < gsize; i++) {
            ImGui::GetBackgroundDrawList()->AddLine(start, end, ImColor(col.Value.x, col.Value.y, col.Value.z, col.Value.w * (1.0f / (float)gsize) * (((float)(gsize - i)) / (float)gsize)), size + i);
        }
    }
    void Crosshair(ImVec4 color, Vector2 center, float size = 20) {
        float x = center.X - (size / 2.0f);
        float y = center.Y - (size / 2.0f);
        Line(ImVec2(x, center.Y), ImVec2(x + size, center.Y), ImVec4(120, 120, 120, 120));
        Line(ImVec2(center.X, y), ImVec2(center.X, y + size), ImVec4(120, 120, 120, 120));
    }
    void BulletTracer(int x, int y, int x2, int y2, int getH) {
        Line(ImVec2(x, getH - y), ImVec2(x2, getH - y2), ImVec4(255, 255, 255, 255), 5.0f);
        Crosshair(ImVec4(77, 111, 192, 255), Vector2(x2, getH - y2), 100);
    }
    void Circle(ImVec4 color, ImVec2 pos, float radius, bool filled) {
        auto background = ImGui::GetBackgroundDrawList();
        if (background) {
            if (filled) {
                background->AddCircleFilled(ImVec2(pos.x, pos.y), radius, ImColor(color.x, color.y, color.z, color.w));
            } else {
                background->AddCircle(ImVec2(pos.x, pos.y), radius, ImColor(color.x, color.y, color.z, color.w));
            }
        }
    }
    void Box(Rect rect, ImVec4 color, float thickness = 3.0f) {
        ImVec2 v1(rect.x, rect.y);
        ImVec2 v2(rect.x + rect.width, rect.y);
        ImVec2 v3(rect.x + rect.width, rect.y + rect.height);
        ImVec2 v4(rect.x, rect.y + rect.height);

        Line(v1, v2, color, thickness);
        Line(v2, v3, color, thickness);
        Line(v3, v4, color, thickness);
        Line(v4, v1, color, thickness);
    }
    void Box2(Rect rect, ImVec4 color, float thickness = 3.0f) {
        ImVec2 v1(rect.x, rect.y); //низ левый
        ImVec2 v11(rect.x + (rect.width / 3), rect.y);
        ImVec2 v12(rect.x, rect.y + (rect.height / 4));
        
        ImVec2 v2(rect.x + rect.width, rect.y); //низ правый
        ImVec2 v21((rect.x + rect.width) - (rect.width / 3), rect.y);
        ImVec2 v22(rect.x + rect.width, rect.y + (rect.height / 4));
        
        ImVec2 v3(rect.x + rect.width, rect.y + rect.height); //верх правый
        ImVec2 v31((rect.x + rect.width) - (rect.width / 3), rect.y + rect.height);
        ImVec2 v32(rect.x + rect.width, (rect.y + rect.height) - (rect.height / 4));
        
        ImVec2 v4(rect.x, rect.y + rect.height); //верх левый
        ImVec2 v41(rect.x + (rect.width / 3), rect.y + rect.height);
        ImVec2 v42(rect.x, (rect.y + rect.height) - (rect.height / 4));

        Line(v1, v11, color, thickness);
        Line(v1, v12, color, thickness);
        Line(v2, v21, color, thickness);
        Line(v2, v22, color, thickness);
        Line(v3, v31, color, thickness);
        Line(v3, v32, color, thickness);
        Line(v4, v41, color, thickness);
        Line(v4, v42, color, thickness);
    }
    void Text2(float fontSize, ImVec2 position, ImVec4 color, const char *text, bool center = true) {
        auto background = ImGui::GetBackgroundDrawList();
        if (background) {
            if (center) {
                ImVec2 textSize = ImGui::CalcTextSize(text);
                ImVec2 centeredPosition = ImVec2(position.x - (textSize.x * 0.5f), position.y - (textSize.y * 0.5f));
                background->AddText(NULL, fontSize, centeredPosition, ImColor(color.x, color.y, color.z, color.w), text);
            } else {
                background->AddText(NULL, fontSize, position, ImColor(color.x, color.y, color.z, color.w), text);
            }
        }
    }
    void TextInCircle(const ImVec2& center, float radius, const char* text) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return;
        
        ImGuiContext& g = *GImGui;
        const ImGuiStyle& style = g.Style;
        const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
        
        const ImVec2 text_size = ImGui::CalcTextSize(text, nullptr, true);
        const float angle_per_letter = 2.0f * IM_PI / (float)strlen(text);
        
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        
        for (int i = 0; i < strlen(text); i++) {
            const float theta = angle_per_letter * i;
            const ImVec2 letter_pos(center.x + cos(theta) * radius - text_size.x * 0.5f, center.y + sin(theta) * radius - text_size.y * 0.5f);
            
            draw_list->AddText(NULL, 0.0f, letter_pos, textCol, text + i, text + i + 1);
        }
    }
    void HorizontalHealthBar(Vector2 screenPos, float width, float maxHealth, float currentHealth, ImVec4 clr) {
        screenPos -= Vector2(0.0f, 8.0f);
        Box(Rect(screenPos.X, screenPos.Y, width + 2, 5.0f), ImVec4(0, 0, 0, 255));
        screenPos += Vector2(1.0f, 1.0f);
        float hpWidth = (currentHealth * width) / maxHealth;
        if (currentHealth <= (maxHealth * 0.6)) {
            clr = ImVec4(255, 255, 0, 255);
        }
        if (currentHealth < (maxHealth * 0.3)) {
            clr = ImVec4(255, 0, 0, 255);
        }
        if (currentHealth <= (maxHealth * 0.0)) {
            clr = ImVec4(0, 0, 0, 0);
        }
        Box(Rect(screenPos.X, screenPos.Y, hpWidth, 3.0f), clr);
    }
    void VerticalHealth(Vector2 end, float h, float health) {
        float x = end.X;
        float y = end.Y;
        h = -h;
        ImVec4 clr = ImVec4(0, 255, 0, 255);

        float hpwidth = h - h * health / 100;

        if (health <= (100 * 0.6)) {
            clr = ImVec4(255, 255, 0, 255);
        }
        if (health < (100 * 0.3)) {
            clr = ImVec4(255, 0, 0, 255);
        }

        Rect hpbar((x + h / 4) - 8, y, 4.0f ,-h);
        Rect hp((x + h / 4) - 8, y - hpwidth, 2.0f, -h + hpwidth);

        Box(hpbar, ImVec4(0, 0, 0, 255), 3);

        Box(hp, clr, 3);
    }
}

ImVec4 RainbowColor(float alpha = 1.f) {
    auto isFrames = ImGui::GetFrameCount();
    static float isRed = 0.f, isGreen = 0.01f, isBlue = 0.f;
    if (isFrames % 5 == 0) {
        if (isGreen == 0.01f && isBlue == 0.f) {
            isRed += 0.01f;
        }
        if (isRed > 0.99f && isBlue == 0.f) {
            isRed = 1.f;
            isGreen += 0.01f;
        }
        if (isGreen > 0.99f && isBlue == 0.f) {
            isGreen = 1.f;
            isRed -= 0.01f;
        }
        if (isRed < 0.01f && isGreen == 1.f) {
            isRed = 0.f;
            isBlue += 0.01f;
        }
        if (isBlue > 0.99f && isRed == 0.f) {
            isBlue = 1.f;
            isGreen -= 0.01f;
        }
        if (isGreen < 0.01f && isBlue == 1.f) {
            isGreen = 0.f;
            isRed += 0.01f;
        }
        if (isRed > 0.99f && isGreen == 0.f) {
            isRed = 1.f;
            isBlue -= 0.01f;
        }
        if (isBlue < 0.01f && isGreen == 0.f) {
            isBlue = 0.f;
            isRed -= 0.01f;
            if (isRed < 0.01f)
                isGreen = 0.01f;
        }
    }
    return ImVec4(isRed, isGreen, isBlue, alpha);
}