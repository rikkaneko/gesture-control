#include <Arduino.h>
#include <SparkFun_APDS9960.h>
#include <BleKeyboard.h>
#include <Wire.h>

// Constants
constexpr uint8_t MODE_N = 6;
constexpr uint8_t ACTION_PER_MODE_N = 6;
constexpr uint8_t STATUS_LED_PIN = 9;
constexpr uint8_t MODE_LED_PINS[MODE_N] = { 1, 2, 3, 4, 5, 6 };
constexpr uint8_t APDS_INT_PIN = 10;
constexpr uint8_t APDS_LED = 11;
// Device interface
SparkFun_APDS9960 apds = SparkFun_APDS9960();
BleKeyboard ble_keyboard("Gesture Control", "3 Poor EIE Guy");

struct KeyAction {
	bool enabled;
	// 3 modifier keys + 3 normal keys
	uint8_t modifier[3];
	uint8_t normal_key;
	const MediaKeyReport *media_key;
	bool is_media_key;
	bool should_hold;
};

// Shared states
uint8_t current_mode = 0;
uint8_t selected_mode = 0;
bool is_adsp_isr = false;
bool is_mode_selection = false;
KeyAction actions[MODE_N][ACTION_PER_MODE_N] = {
	// Mode 0
	{
		// Action 0 (Left) Play/Pause / ACCEPT A CALL
		{ 1,{}, 0, &KEY_MEDIA_PLAY_PAUSE, 1, 0 },
		// Action 1 (Right) Stop / DENY A CALL
		{ 1, {}, 0, &KEY_MEDIA_STOP, 1, 0 },
		// Action 2 (Up) Previous track
		{ 1, {}, 0, &KEY_MEDIA_PREVIOUS_TRACK, 1, 0 },
		// Action 3 (Down) Next track
		{ 1, {}, 0, &KEY_MEDIA_NEXT_TRACK, 1, 0 }
	},
	// Mode 1
	{
		// Action 0 (Left) Play/Pause / ACCEPT A CALL
		{ 1, {}, 0, &KEY_MEDIA_PLAY_PAUSE, 1, 0 },
		// Action 1 (Right) Mute
		{ 1, {}, 0, &KEY_MEDIA_MUTE, 1, 0 },
		// Action 2 (Up) Page Up
		{ 1, {}, KEY_PAGE_UP, nullptr, 0, 0 },
		// Action 3 (Down) Page Down
		{ 1, {}, KEY_PAGE_UP, nullptr, 0, 0 }
	},
	// Mode 2
	{
		// Action 0 (Left) Left
		{ 1, {}, KEY_LEFT_ARROW, nullptr, 0, 0 },
		// Action 1 (Right) Right
		{ 1, {}, KEY_RIGHT_ARROW, nullptr, 0, 0 },
		// Action 2 (Up) Up
		{ 1, {}, KEY_UP_ARROW, nullptr, 0, 0 },
		// Action 3 (Down) Down
		{ 1, {}, KEY_DOWN_ARROW, nullptr, 0, 0 }
	},
	// Mode 2
	{
		// Action 0 (Left) Left
		{ 1, {}, 'A', nullptr, 0, 1 },
		// Action 1 (Right) Right
		{ 1, {}, 'D', nullptr, 0, 1 },
		// Action 2 (Up) Up
		{ 1, {}, 'W', nullptr, 0, 1 },
		// Action 3 (Down) Down
		{ 1, {}, 'S', nullptr, 0, 1 }
	},
};

void blink_led();
void blink_panic();
void handle_gesture();

void blink_led() {
	int saved = digitalRead(STATUS_LED_PIN);
	for (int i = 0, curr = 0; i < 4; ++i, curr ^= 1) {
		digitalWrite(STATUS_LED_PIN, curr);
		delay(100);
	}
	digitalWrite(STATUS_LED_PIN, saved);
}

void blink_apds_led() {
	for (int i = 0, curr = 0; i < 3; ++i, curr ^= 1) {
		digitalWrite(STATUS_LED_PIN, curr);
		delay(250);
	}
}

void blink_panic() {
	uint8_t curr = 0;
	while (true) {
		digitalWrite(STATUS_LED_PIN, curr);
		delay(100);
		curr ^= 1;
	}
}

void set_mode_led() {
	uint8_t mode = (is_mode_selection) ? selected_mode : current_mode;
	for (uint8_t i: MODE_LED_PINS) digitalWrite(i, 0);
	digitalWrite(MODE_LED_PINS[mode], 1);
}

uint8_t map_apds_gesture(int gesture) {
	return (gesture)? gesture - 1: 0;
}

void setup() {
	pinMode(STATUS_LED_PIN, OUTPUT);
	pinMode(APDS_INT_PIN, INPUT_PULLUP);
	for (unsigned char i: MODE_LED_PINS) pinMode(i, OUTPUT);
	Serial.begin(115200);
	// Wait for serial connection for 3 second
	auto t = millis();
	while (!Serial && millis() - t < 3000);
	blink_led();
	// Initialize APDS-9960
	if (!apds.init()) {
		Serial.println("[ERROR] APDS-9960 initialization failed");
		blink_panic();
	}
	if (!apds.enableGestureSensor(true))
		Serial.println("[ERROR] Cannot setup the gesture sensor");
	if (!apds.setGestureGain(1))
		Serial.println("[ERROR] Cannot configure gesture's gain");
	// Initialize the BLE HID Keyboard
	ble_keyboard.begin();
	attachInterrupt(APDS_INT_PIN, []() { is_adsp_isr = true; }, FALLING);
}

void handle_gesture() {
	if (apds.isGestureAvailable()) {
		int gesture = apds.readGesture();
		// Mode selection
		if (is_mode_selection) {
			switch (gesture) {
			// Cancel mode selection
			case DIR_FAR:
				is_mode_selection = false;
				break;
			// Confirm mode selection
			case DIR_NEAR:
				is_mode_selection = true;
				current_mode = selected_mode;
				break;
			// Previous mode
			case DIR_LEFT:
				selected_mode = (selected_mode == 0) ? MODE_N - 1 : selected_mode - 1;
				break;
			// Next mode
			case DIR_RIGHT:
				selected_mode = (selected_mode + 1) % MODE_N;
				break;
			default:
				break;
			}
			set_mode_led();
			return;
		}
		
		// Enter mode selection mode
		if (gesture == DIR_FAR) {
			is_mode_selection = true;
			selected_mode = current_mode;
			return;
		}
		
		// Normal action
		ble_keyboard.releaseAll();
		uint8_t val = map_apds_gesture(gesture);
		KeyAction action = actions[current_mode][val];
		if (!action.enabled) return;
		for (uint8_t i: action.modifier) ble_keyboard.press(i);
		if (action.is_media_key) ble_keyboard.press(*action.media_key);
		else ble_keyboard.press(action.normal_key);
		if (!action.should_hold) ble_keyboard.releaseAll();
	}
}

void loop() {
	if (ble_keyboard.isConnected()) {
		if (is_adsp_isr) {
			detachInterrupt(APDS_INT_PIN);
			handle_gesture();
			blink_apds_led();
			is_adsp_isr = false;
			attachInterrupt(APDS_INT_PIN, []() { is_adsp_isr = true; }, FALLING);
		}
	}
}
