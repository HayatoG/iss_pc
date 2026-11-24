// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "imgui.h"
namespace OverlayDesign {
struct MenuStyle {
    explicit MenuStyle(float scale) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * scale, 16 * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * scale, 5 * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12 * scale, 8 * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(8 * scale, 4 * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(10 * scale, 5 * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8 * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5 * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 12 * scale);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.035f, .055f, .09f, .98f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.055f, .08f, .125f, .72f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.94f, .96f, 1.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(.62f, .70f, .80f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(.28f, .40f, .56f, .40f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(.12f, .18f, .27f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(.18f, .29f, .43f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(.20f, .36f, .55f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.15f, .25f, .38f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.22f, .39f, .60f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.24f, .46f, .73f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(.16f, .34f, .55f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(.21f, .41f, .65f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(.25f, .47f, .73f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(.44f, .77f, 1.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(.42f, .73f, .98f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(.25f, .40f, .58f, .075f));
    }
    ~MenuStyle() { ImGui::PopStyleColor(17); ImGui::PopStyleVar(9); }
};
inline void Section(const char* title) {
    if (ImGui::GetCursorPosY() > ImGui::GetStyle().WindowPadding.y + ImGui::GetTextLineHeight()) ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * .22f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.55f, .78f, 1.f, 1.f));
    ImGui::SeparatorText(title); ImGui::PopStyleColor();
}
inline void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text); ImGui::PopStyleColor();
}
}
