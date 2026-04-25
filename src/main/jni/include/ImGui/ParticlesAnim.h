#include <cmath>

void AnimParticles() {
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    ImVec2 cursorPos = ImGui::GetIO().MousePos;
    float deltaTime = ImGui::GetIO().DeltaTime;
	static const int numParticles = 115;
	static ImVec2 particlePositions[numParticles];
	static ImVec2 particleDistance;
	static ImVec2 particleVelocities[numParticles];
    static bool initialized = false;
	if (!initialized) {
		for (int i = 0; i < numParticles; ++i) {
			particlePositions[i] = ImVec2(
				ImGui::GetWindowPos().x + ImGui::GetWindowSize().x * static_cast<float>(rand()) / RAND_MAX,
				ImGui::GetWindowPos().y + ImGui::GetWindowSize().y * static_cast<float>(rand()) / RAND_MAX
			);
			particleVelocities[i] = ImVec2(
				static_cast<float>((rand() % 51) - 5),
				static_cast<float>((rand() % 51) - 5)
			);
		}
		initialized = true;
	}
	for (int i = 0; i < numParticles; ++i) {
		for (int j = i + 1; j < numParticles; ++j) {
			float distance = std::hypotf(particlePositions[j].x - particlePositions[i].x, particlePositions[j].y - particlePositions[i].y);
			float opacity = 1.0f - (distance / 55.0f);
			if (opacity > 0.0f) {
				ImU32 lineColor = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, opacity));
				drawList->AddLine(particlePositions[i], particlePositions[j], lineColor);
			}
		}
		float distanceToCursor = std::hypotf(cursorPos.x - particlePositions[i].x, cursorPos.y - particlePositions[i].y);
		float opacityToCursor = 1.0f - (distanceToCursor / 52.0f);
		if (opacityToCursor > 0.0f) {
			ImU32 lineColorToCursor = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, opacityToCursor));
			drawList->AddLine(cursorPos, particlePositions[i], lineColorToCursor);
		}
	}
	for (int i = 0; i < numParticles; ++i) {
		particlePositions[i].x += particleVelocities[i].x * deltaTime;
		particlePositions[i].y += particleVelocities[i].y * deltaTime;
		if (particlePositions[i].x < ImGui::GetWindowPos().x)
			particlePositions[i].x = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x;
		else if (particlePositions[i].x > ImGui::GetWindowPos().x + ImGui::GetWindowSize().x)
			particlePositions[i].x = ImGui::GetWindowPos().x;
		if (particlePositions[i].y < ImGui::GetWindowPos().y)
			particlePositions[i].y = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
		else if (particlePositions[i].y > ImGui::GetWindowPos().y + ImGui::GetWindowSize().y)
			particlePositions[i].y = ImGui::GetWindowPos().y;
		ImU32 particleColour = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
		drawList->AddCircleFilled(particlePositions[i], 1.5f, particleColour);
	}
}