// openhouse-installer: installs and uninstalls the openhouse driver, and flashes openhouse's
// firmware (1.0 base stations, doorman dongles, trackers' and controllers' radios), with a guided
// window for each. Windows and Linux; everything it needs is linked in, and the firmware files
// come from the package's firmwares/ folder.
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl2.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "logo_rgba.h"
#include "ui.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace openhouse::installer;

GLuint UploadRgba(const uint8_t* rgba, int w, int h) {
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  return texture;
}

int Run() {
  if (!glfwInit()) {
#ifdef _WIN32
    MessageBoxW(nullptr, L"openhouse-installer couldn't open its window.", L"openhouse", MB_ICONERROR);
#else
    std::fprintf(stderr, "openhouse-installer: no display\n");
#endif
    return 1;
  }
  GLFWwindow* window = glfwCreateWindow(1000, 720, "openhouse installer", nullptr, nullptr);
  if (!window) { glfwTerminate(); return 1; }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);
  GLFWimage icon{openhouse::overlay::kLogoSize, openhouse::overlay::kLogoSize, const_cast<unsigned char*>(openhouse::overlay::kLogoRgba)};
  glfwSetWindowIcon(window, 1, &icon);
  ImGui::CreateContext();
  Ui::SetUpImGui();
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL2_Init();
  {
    Ui ui;
    ui.SetLogo(UploadRgba(openhouse::overlay::kLogoRgba, openhouse::overlay::kLogoSize, openhouse::overlay::kLogoSize));
    while (!glfwWindowShouldClose(window)) {
      glfwWaitEventsTimeout(1.0 / 30);  // keeps redrawing for progress bars and device lists
      ImGui_ImplOpenGL2_NewFrame();
      ImGui_ImplGlfw_NewFrame();
      ImGui::NewFrame();
      ui.Draw(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
      ImGui::Render();
      int w, h;
      glfwGetFramebufferSize(window, &w, &h);
      glViewport(0, 0, w, h);
      glClearColor(0.06f, 0.06f, 0.07f, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
      glfwSwapBuffers(window);
    }
  }
  // Scans finish on their own threads: let the last one end before the process does (bounded).
  std::unique_lock devices(openhouse::installer::g_deviceMutex, std::defer_lock);
  for (int i = 0; i < 100 && !devices.try_lock(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ImGui_ImplOpenGL2_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
}  // namespace

int main() { return Run(); }
