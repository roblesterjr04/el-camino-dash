// Host tests for the console's behaviour (console_ui.cpp): the prototype's knob / key model, the OLED menu's live
// edit and undo, the round-screen menu driven over CAN, warnings, and the call soft keys. Run: ./run_tests.sh

#include <cstdio>
#include <cstring>

#include "console_ui.h"

static int g_failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failures++;                                                 \
    }                                                               \
  } while (0)

static uint32_t T = 10000;

static void center(ConsoleUi &ui, uint8_t view = 0) {
  ElcCanFrame f;
  f.id = ELC_CAN_ID_HEARTBEAT_BASE + ELC_CAN_NODE_CENTER;
  f.len = 8;
  ui.feed(f, T);
  ElcCanClusterState c;
  c.center_subview = view;
  elc_can_encode_cluster_state(c, f);
  ui.feed(f, T);
}

static void sensors(ConsoleUi &ui, uint16_t oil_adc = 409) {
  ElcCanSensor s;
  s.engine_rpm = 2200;
  s.pot_adc = 593;
  s.pressure_adc = oil_adc;
  s.therm_adc = 560;
  s.flags = 0x03;
  s.odometer_tenths_mile = 1000;
  ElcCanFrame f[ELC_CAN_SENSOR_FRAMES];
  elc_can_encode_sensor(s, f);
  for (auto &x : f) ui.feed(x, T);
}

/** A console that has booted and hears the center and the sensors. */
static void boot(ConsoleUi &ui) {
  ui.tick(T);
  T += 1500;
  center(ui);
  sensors(ui);
  ui.tick(T);
}

static bool pop(ConsoleUi &ui, ElcCanFrame &f) {
  ConsoleAction a;
  if (!ui.pop_action(a)) return false;
  f = a.frame;
  return true;
}

static void drain(ConsoleUi &ui) {
  ElcCanFrame f;
  while (pop(ui, f)) {
  }
}

static void test_drive_and_menu() {
  std::printf("drive pages, OLED menu, live edit and undo\n");
  ConsoleUi ui;
  boot(ui);
  CHECK(ui.alive(T) && ui.mode() == ConsoleMode::Drive && ui.page() == kPageMedia);
  ui.select_turn(1, T);
  CHECK(ui.page() == kPageTrip);
  ui.select_turn(-1, T);
  ui.select_turn(-1, T);
  CHECK(ui.page() == kPageClock);  // wraps
  ui.back(T);
  CHECK(ui.page() == kPageMedia);  // Back = Now Playing

  ui.select_press(T);
  CHECK(ui.mode() == ConsoleMode::Menu && ui.menu_sel() == 0);
  ui.select_turn(1, T);  // Theme
  ui.select_press(T);
  CHECK(ui.mode() == ConsoleMode::Edit);
  drain(ui);
  ui.select_turn(1, T);  // Classic -> Modern, sent at once
  ElcCanFrame f;
  ElcCanSettingRequest r;
  CHECK(pop(ui, f) && elc_can_decode_setting_request(f, r) && r.setting == ELC_CAN_SET_THEME && r.value == 2);
  ui.back(T);  // undo: back to Classic
  CHECK(pop(ui, f) && elc_can_decode_setting_request(f, r) && r.setting == ELC_CAN_SET_THEME && r.value == 0);
  CHECK(ui.mode() == ConsoleMode::Menu);
  ui.back(T);
  CHECK(ui.mode() == ConsoleMode::Drive);

  // Trip A: push, push again to confirm resets it to the odometer.
  ui.select_press(T);
  ui.select_turn(-20, T);  // the menu remembers where it was (as on the prototype): to the top, then Trip A
  ui.select_turn(7, T);
  ui.select_press(T);
  CHECK(ui.mode() == ConsoleMode::Edit);
  ui.select_press(T);
  CHECK(ui.trip_set() && ui.trip_base() == 1000 && ui.take_persist_dirty());
}

static void test_remote_menu() {
  std::printf("the round-screen menu driven over CAN\n");
  ConsoleUi ui;
  boot(ui);
  ui.select_press(T);
  ui.select_turn(9, T);  // Service
  drain(ui);
  ui.select_press(T);  // opens that page on the interface screen
  ElcCanFrame f;
  ElcCanMenuInput m;
  CHECK(pop(ui, f) && elc_can_decode_menu_input(f, m) && m.event == ELC_CAN_MENU_OPEN && m.value == ELC_CAN_PAGE_SERVICE);
  CHECK(ui.mode() == ConsoleMode::Remote);
  ElcCanMenuState st;
  st.open = true;
  st.page = ELC_CAN_PAGE_SERVICE;
  st.opened_by = ELC_CAN_MENU_BY_CONSOLE;
  elc_can_encode_menu_state(st, f);
  ui.feed(f, T);
  ui.tick(T);
  ui.select_turn(-2, T);
  CHECK(pop(ui, f) && elc_can_decode_menu_input(f, m) && m.event == ELC_CAN_MENU_ROTATE && m.value == -2);
  ui.back(T);
  CHECK(pop(ui, f) && elc_can_decode_menu_input(f, m) && m.event == ELC_CAN_MENU_BACK);
  st.open = false;
  elc_can_encode_menu_state(st, f);
  ui.feed(f, T);
  ui.tick(T);
  CHECK(ui.mode() == ConsoleMode::Menu);  // opened from the OLED menu: back to it

  // Opened by touch: the knob follows it, and returns to the drive pages when it closes.
  ui.back(T);
  CHECK(ui.mode() == ConsoleMode::Drive);
  st.open = true;
  st.opened_by = ELC_CAN_MENU_BY_TOUCH;
  st.page = ELC_CAN_PAGE_HOME;
  elc_can_encode_menu_state(st, f);
  ui.feed(f, T);
  ui.tick(T);
  CHECK(ui.mode() == ConsoleMode::Remote);
  st.open = false;
  elc_can_encode_menu_state(st, f);
  ui.feed(f, T);
  ui.tick(T);
  CHECK(ui.mode() == ConsoleMode::Drive);
}

static void test_alerts() {
  std::printf("warnings take over until acknowledged, and re-arm\n");
  ConsoleUi ui;
  boot(ui);
  CHECK(ui.alert(T) == nullptr);
  sensors(ui, 50);  // ~5 psi at 2200 rpm
  const ConsoleAlert *a = ui.alert(T);
  CHECK(a && !strcmp(a->id, "oil"));
  const uint8_t page = ui.page();
  ui.select_turn(1, T);
  CHECK(ui.page() == page);  // turning does not move pages under a warning
  ui.select_press(T);        // acknowledge
  CHECK(ui.alert(T) == nullptr && ui.mode() == ConsoleMode::Drive);
  sensors(ui, 409);
  CHECK(ui.alert(T) == nullptr);
  sensors(ui, 50);
  CHECK(ui.alert(T) != nullptr);  // cleared and came back: shown again
}

static void phone(ConsoleUi &ui, uint8_t call, uint8_t flags = 0) {
  ElcCanPhoneStatus s;
  s.flags = (uint8_t)(ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP | flags);
  s.call = call;
  ElcCanFrame f;
  elc_can_encode_phone_status(1, s, f);
  ui.feed(f, T);
}

static bool pop_phone(ConsoleUi &ui, uint8_t cmd, uint8_t arg) {
  ElcCanFrame f;
  ElcCanPhoneCommandMsg c;
  return pop(ui, f) && elc_can_decode_phone_command(f, c) && c.command == cmd && c.arg == arg && c.slot == 1;
}

static void test_calls() {
  std::printf("calls: soft keys on Back / View\n");
  ConsoleUi ui;
  boot(ui);
  drain(ui);
  phone(ui, ELC_CAN_CALL_INCOMING);
  CHECK(ui.call_shown(T));
  ui.select_press(T);  // the knob does not answer by accident
  ConsoleAction none;
  CHECK(!ui.pop_action(none));
  ui.view(T);
  CHECK(pop_phone(ui, ELC_CAN_PHONE_CMD_ANSWER, 0));
  ui.back(T);
  CHECK(pop_phone(ui, ELC_CAN_PHONE_CMD_END, 0));
  phone(ui, ELC_CAN_CALL_ACTIVE);
  ui.view(T);
  CHECK(pop_phone(ui, ELC_CAN_PHONE_CMD_MIC_MUTE, 1));
  phone(ui, ELC_CAN_CALL_ACTIVE, ELC_CAN_PHONE_MIC_MUTED);
  ui.view(T);
  CHECK(pop_phone(ui, ELC_CAN_PHONE_CMD_MIC_MUTE, 0));
  ui.volume_turn(2, T);
  CHECK(pop_phone(ui, ELC_CAN_PHONE_CMD_CALL_VOLUME, 2));
  phone(ui, ELC_CAN_CALL_WAITING);
  ui.back(T);
  CHECK(pop_phone(ui, ELC_CAN_PHONE_CMD_CALL_WAITING, 0));
  ui.view(T);
  CHECK(pop_phone(ui, ELC_CAN_PHONE_CMD_CALL_WAITING, 2));
  phone(ui, ELC_CAN_CALL_IDLE);
  CHECK(!ui.call_shown(T));
  ui.view(T);  // no call: View is the next center view again
  ElcCanFrame f;
  ElcCanSettingRequest r;
  CHECK(pop(ui, f) && elc_can_decode_setting_request(f, r) && r.setting == ELC_CAN_SET_CENTER_VIEW && r.value == 1);
}

static void test_media_keys() {
  std::printf("volume knob: volume, play / pause, hold = next\n");
  ConsoleUi ui;
  boot(ui);
  drain(ui);
  ui.volume_turn(3, T);
  drain(ui);
  ui.volume_press(T);
  ElcCanFrame f;
  ElcCanPhoneCommandMsg c;
  CHECK(pop(ui, f) && elc_can_decode_phone_command(f, c) && c.command == ELC_CAN_PHONE_CMD_MEDIA && c.arg == 0);
  ui.volume_hold(T);
  CHECK(pop(ui, f) && elc_can_decode_phone_command(f, c) && c.command == ELC_CAN_PHONE_CMD_MEDIA && c.arg == 1);
}

static void test_no_center() {
  std::printf("nothing from the center: inert\n");
  ConsoleUi ui;
  ui.tick(T);
  T += 1500;
  ui.tick(T);
  CHECK(!ui.alive(T));
  ui.select_press(T);
  ui.view(T);
  ConsoleAction a;
  CHECK(!ui.pop_action(a) && ui.mode() == ConsoleMode::Drive);
}

int main() {
  test_drive_and_menu();
  test_remote_menu();
  test_alerts();
  test_calls();
  test_media_keys();
  test_no_center();
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("all console UI tests passed\n");
  return 0;
}
