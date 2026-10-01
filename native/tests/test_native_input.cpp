/*
 * test_native_input.cpp - Host keys reach the machine as the browser's would
 *
 * The native front end's keyboard is a translation to browser keycodes and
 * nothing else; the core does the rest. So the test is the one a user makes:
 * press keys, through the mapping, into a real //e, and read its screen.
 *
 * Also pins the two queues the emulation thread hands work through.
 */

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "../src/audio_ring.hpp"
#include "../src/frame_queue.hpp"
#include "../src/game_port.hpp"
#include "../src/key_mapper.hpp"
#include "../../src/host/machine_host.hpp"

#include <string>
#include <vector>

using namespace a2e;
using namespace a2e::native;

namespace {

void runFrames(host::MachineHost &host, int frames) {
  std::vector<float> buffer(800 * 2);
  for (int i = 0; i < frames; i++) host.generateStereoAudioSamples(buffer.data(), 800);
}

// One key down and up, as App::routeKeyboard sends it.
void press(host::MachineHost &host, ImGuiKey key, HeldModifiers held,
           bool commandIsOpenApple = false) {
  const auto hostKey = browserKeyFor(key, true);
  REQUIRE(hostKey);
  const auto down = coreKeyEvent(*hostKey, held, commandIsOpenApple, false);
  REQUIRE(down);
  host.handleRawKeyDown(down->keyCode, down->shift, down->ctrl, down->alt,
                        down->meta, false, down->location);
  runFrames(host, 2);
  const auto up = coreKeyEvent(*hostKey, held, commandIsOpenApple, true);
  REQUIRE(up);
  host.handleRawKeyUp(up->keyCode, up->shift, up->ctrl, up->alt, up->meta,
                      up->location);
  runFrames(host, 2);
}

ImGuiKey keyFor(char c) {
  if (c >= 'A' && c <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (c - 'A'));
  if (c >= '0' && c <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (c - '0'));
  if (c == ' ') return ImGuiKey_Space;
  if (c == '=') return ImGuiKey_Equal; // shifted, it is +
  return ImGuiKey_None;
}

std::string screen(host::MachineHost &host) {
  return host.emulator()->readScreenText(0, 0, 23, 39);
}

int count(const std::string &text, char c) {
  int n = 0;
  for (char ch : text) n += ch == c;
  return n;
}

} // namespace

TEST_CASE("Keys typed through the mapping reach a //e as typed",
          "[native][keyboard]") {
  host::MachineHost host;
  host.build();
  runFrames(host, 30);
  host.warmReset(); // Ctrl+Reset to the Applesoft prompt
  runFrames(host, 30);
  const int promptsBefore = count(screen(host), ']');

  HeldModifiers shifted;
  shifted.shift = true;
  for (char c : std::string("PRINT ")) press(host, keyFor(c), shifted);
  press(host, ImGuiKey_2, HeldModifiers{});
  press(host, ImGuiKey_Equal, shifted); // Shift and = is +
  press(host, ImGuiKey_3, HeldModifiers{});
  press(host, ImGuiKey_Enter, HeldModifiers{});
  runFrames(host, 30);

  const std::string text = screen(host);
  INFO("screen:\n" << text);
  REQUIRE(text.find("PRINT 2+3") != std::string::npos);
  REQUIRE(text.find("\n5") != std::string::npos);
  // One Return, one new prompt.
  REQUIRE(count(text, ']') == promptsBefore + 1);
}

TEST_CASE("Unshifted letters are lower case on a //e", "[native][keyboard]") {
  host::MachineHost host;
  host.build();
  runFrames(host, 30);
  host.warmReset();
  runFrames(host, 30);
  press(host, ImGuiKey_A, HeldModifiers{});
  const std::string text = screen(host);
  INFO("screen:\n" << text);
  REQUIRE(text.find("]a") != std::string::npos);
}

TEST_CASE("Mac modifier keys map to the browser's", "[native][keyboard]") {
  // With ImGui's Mac behaviour, Ctrl is the physical Command key.
  REQUIRE(browserKeyFor(ImGuiKey_LeftCtrl, true)->keyCode == KEY_META_LEFT);
  REQUIRE(browserKeyFor(ImGuiKey_LeftSuper, true)->keyCode == KEY_CONTROL);
  REQUIRE(browserKeyFor(ImGuiKey_LeftCtrl, false)->keyCode == KEY_CONTROL);
  REQUIRE(browserKeyFor(ImGuiKey_RightAlt, true)->location == LOCATION_RIGHT);
  REQUIRE(browserKeyFor(ImGuiKey_Keypad5, true)->keyCode == 101);
  REQUIRE_FALSE(browserKeyFor(ImGuiKey_MouseLeft, true));
}

TEST_CASE("Which host key is Open Apple follows the setting",
          "[native][keyboard]") {
  HeldModifiers command;
  command.command = true;
  HeldModifiers option;
  option.option = true;
  const HostKey leftCommand{KEY_META_LEFT, LOCATION_LEFT};
  const HostKey leftOption{KEY_ALT, LOCATION_LEFT};
  const HostKey rightOption{KEY_ALT, LOCATION_RIGHT};

  SECTION("Option is the Apple keys, Cmd is the app's") {
    auto event = coreKeyEvent(leftOption, option, false, false);
    REQUIRE(event->keyCode == KEY_ALT);
    REQUIRE(event->location == LOCATION_LEFT);
    REQUIRE(event->alt);
    REQUIRE_FALSE(coreKeyEvent(leftCommand, command, false, false));
    // Typed under Cmd: a shortcut, not a key for the machine.
    REQUIRE_FALSE(coreKeyEvent(HostKey{65}, command, false, false));
    // But a key let go under Cmd still comes up.
    REQUIRE(coreKeyEvent(HostKey{65}, command, false, true));
  }

  SECTION("Cmd is Open Apple and either Option is Closed Apple") {
    auto open = coreKeyEvent(leftCommand, command, true, false);
    REQUIRE(open->keyCode == KEY_ALT);
    REQUIRE(open->location == LOCATION_LEFT);
    REQUIRE(open->alt);
    REQUIRE_FALSE(open->meta);
    auto closed = coreKeyEvent(leftOption, option, true, false);
    REQUIRE(closed->location == LOCATION_RIGHT);
    closed = coreKeyEvent(rightOption, option, true, false);
    REQUIRE(closed->location == LOCATION_RIGHT);
    // Cmd-letter goes to the machine, with an Apple key held.
    auto letter = coreKeyEvent(HostKey{65}, command, true, false);
    REQUIRE(letter);
    REQUIRE(letter->alt);
  }
}

TEST_CASE("Cmd as Open Apple presses the //e's Open Apple button",
          "[native][keyboard]") {
  host::MachineHost host;
  host.build();
  HeldModifiers command;
  command.command = true;
  const HostKey leftCommand{KEY_META_LEFT, LOCATION_LEFT};
  auto down = coreKeyEvent(leftCommand, command, true, false);
  host.handleRawKeyDown(down->keyCode, down->shift, down->ctrl, down->alt,
                        down->meta, false, down->location);
  REQUIRE((host.emulator()->peekMemory(0xC061) & 0x80) != 0);
  auto up = coreKeyEvent(leftCommand, HeldModifiers{}, true, true);
  host.handleRawKeyUp(up->keyCode, up->shift, up->ctrl, up->alt, up->meta,
                      up->location);
  REQUIRE((host.emulator()->peekMemory(0xC061) & 0x80) == 0);
}

TEST_CASE("The frame queue shows frames in order and never overruns the held one",
          "[native][frames]") {
  FrameQueue queue;
  uint8_t pixel[4] = {1, 0, 0, 0};
  REQUIRE(queue.take() == nullptr);

  REQUIRE(queue.push(pixel, 1, 1));
  pixel[0] = 2;
  REQUIRE(queue.push(pixel, 1, 1));
  // Two arrived together: shown one refresh apart, oldest first.
  REQUIRE(queue.take()->pixels[0] == 1);
  REQUIRE(queue.take()->pixels[0] == 2);
  REQUIRE(queue.take() == nullptr);

  // At most SLOTS - 2 wait unshown, so the producer never reaches the slot
  // the consumer is holding.
  int pushed = 0;
  while (queue.push(pixel, 1, 1)) pushed++;
  REQUIRE(pushed == FrameQueue::SLOTS - 1);
}

TEST_CASE("The frame queue jumps to the newest past its backlog",
          "[native][frames]") {
  FrameQueue queue;
  uint8_t pixel[4] = {0, 0, 0, 0};
  for (uint8_t i = 1; i <= 3; i++) {
    pixel[0] = i;
    REQUIRE(queue.push(pixel, 1, 1));
  }
  REQUIRE(queue.take()->pixels[0] == 3);
}

TEST_CASE("The audio ring plays what was written and silence past it",
          "[native][audio]") {
  AudioRing ring(8);
  const float in[6] = {1, 2, 3, 4, 5, 6};
  REQUIRE(ring.write(in, 3) == 3);
  float out[8] = {};
  REQUIRE(ring.read(out, 4) == 3);
  REQUIRE(out[0] == 1);
  REQUIRE(out[5] == 6);
  REQUIRE(out[6] == 0);
  REQUIRE(out[7] == 0);

  SECTION("a full ring drops the rest") {
    float many[20] = {};
    REQUIRE(ring.write(many, 10) == 8);
  }
  SECTION("clear takes effect at the reader's next read") {
    ring.write(in, 3);
    ring.clear();
    REQUIRE(ring.read(out, 4) == 0);
  }
}

TEST_CASE("A gamepad's stick is a paddle, past the deadzone", "[native][gameport]") {
  // Inside the deadzone is the middle; the rest is rescaled, so a full push
  // is still a full reading.
  REQUIRE(applyDeadzone(0.05f, 0.1f) == 0.0f);
  REQUIRE(applyDeadzone(1.0f, 0.1f) == Approx(1.0f));
  REQUIRE(applyDeadzone(-0.55f, 0.1f) == Approx(-0.5f));
  REQUIRE(paddleFromAxis(0.0f, 0.1f) == 128);
  REQUIRE(paddleFromAxis(-1.0f, 0.1f) == 0);
  REQUIRE(paddleFromAxis(1.0f, 0.1f) == 255);
}

TEST_CASE("A Joyport stick closes its switches as a CX40 does", "[native][gameport]") {
  Pad pad;
  REQUIRE(joyportMask(pad, 0.1f) == 0);
  // The D-pad, or the stick past halfway, closes the same switch.
  pad.buttons[PAD_UP] = true;
  REQUIRE(joyportMask(pad, 0.1f) == SWITCH_UP);
  pad.buttons[PAD_UP] = false;
  pad.axes[0] = 0.4f; // past the deadzone, short of the switch
  REQUIRE(joyportMask(pad, 0.1f) == 0);
  pad.axes[0] = 0.8f;
  REQUIRE(joyportMask(pad, 0.1f) == SWITCH_RIGHT);
  pad.buttons[PAD_B] = true;
  REQUIRE(joyportMask(pad, 0.1f) == (SWITCH_RIGHT | SWITCH_FIRE));
  // An impossible pair is dropped rather than sent.
  pad.buttons[PAD_LEFT] = true;
  REQUIRE(joyportMask(pad, 0.1f) == SWITCH_FIRE);
}

TEST_CASE("The cursor keys are a stick at full deflection", "[native][gameport]") {
  REQUIRE(paddleFromKeys(false, false) == 128);
  REQUIRE(paddleFromKeys(true, false) == 0);
  REQUIRE(paddleFromKeys(false, true) == 255);
  REQUIRE(paddleFromKeys(true, true) == 128);
  REQUIRE(joyportMaskFromKeys(true, false, true, false) == (SWITCH_UP | SWITCH_LEFT));
}

TEST_CASE("The Joyport's switches reach a //e's pushbuttons, active low", "[native][gameport]") {
  host::MachineHost host;
  host.build();
  host.setGamePortDevice(GamePortDevice::SiriusJoyport);
  runFrames(host, 30); // past the reset guard that lets go of PB0/PB1
  host.setJoyportStick(0, SWITCH_FIRE);
  // Stick 1, AN0 and AN1 off: PB0 is fire, and a closed switch reads low.
  REQUIRE((host.emulator()->peekMemory(0xC061) & 0x80) == 0);
  host.setJoyportStick(0, 0);
  REQUIRE((host.emulator()->peekMemory(0xC061) & 0x80) != 0);
}
