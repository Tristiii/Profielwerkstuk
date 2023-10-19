// === Project documentation ===
// By Lucas van Osenbruggen & Emile Verheijen
// Contact: lucasvanosenbruggen@gmail.com
//
// TODO:
// Test
// Write manual
// Calibrate water (stepper)
// Better input validation/clearer questions
// Document project
// Store sensors in eeprom
// 
// Improvements:
// ~Account for lamps of different sizes
// ~Tube fill time
// Sensors: temperature, air humidity, ground humidity
// Heating
// ~Stepper motor
// Time active display
// ~Sensor displays
// Keep moist mode
// Export sensor data
// Output current schedule


// === Initialise variables ===
// == Fixed values ==
#define temp_pin A1
#define temp_low 0
#define temp_high 50

#define air_humid_pin A0
#define air_humid_low 20
#define air_humid_high 90

#define humid_pins 4
int humid[humid_pins] = {A2, A3, A4, A5}; // TODO add

#define pump_dir 13
#define pump_step 12
#define pump_step_size 200 // Steps per rotation, fixed for a stepper motor
#define pump_fill_time = 5000; // Time it takes to fill the tubes with water WARNING not accurate
#define pump_type 'D' // D = DC-motor, S = stepper-motor
int pump_max_flowrate; // Based on chosen motor

#define lamp_amount 14
#define lamp_sub_units = 68; // Most lamps have 5 subunits but some have 4 WARNING not in use
#define lamp_pins 3
int lampA[lamp_pins] = {5, 6, 7}; // Each pin controls a relay, which controls one or more lights
// Lamps connected: 1  2  4
// Groups:          A______
int lampB[lamp_pins] = {2, 3, 4};
// Lamps connected: 1  2  4
// Groups:          B______
//                  C______ (A + B)

#define switch_rate 3600000 // Valves will switch within this timeframe (1h)
// #define switch_rate = 240000; // TEST shorter switch rate (8min)
#define valve_amount 8
#define valve_bits 3
int valve[valve_bits] = {8, 9, 10}; // Forms a 3-bit binary number using these pins
// Binary:  000 001 010 011 100 101 110 111
// Valve:   0   1   2   3   4   5   6   7
// Groups:  B1  B2  B3  B4  A1  A2  A3  A4
//          Bx____________  Ax____________
//          Cx____________________________ (Ax + Bx)


// == Changing values ==
unsigned long t_prev = 0;
unsigned int inp_index = 0;

// Will store time of compilation
unsigned int t_start_hour;
unsigned int t_start_minute;

// Time storage
unsigned long t_minutes = 0;
unsigned int t_real_minute;
unsigned int t_hours = 0;
unsigned int t_real_hour;
unsigned int t_days = 0;

// For async delays
unsigned long t_pump = 0;
unsigned long t_valve = 0;

// Data collect
#define data_points 50
int data_point = 0;
int temp[data_points];
int air_humid[data_points];
int gnd_humidity[humid_pins][data_points];

// == Changable settings ==
bool data_col = false; // Gather data
bool control_fixed = true; // Constantly use the same value or use a schedule

// User chooses which parts to operate, they can be mentioned by groups
char control_lamps[2] = {'C'}; // [A, B, C = A,B]
String control_valves[8] = {"Cx"}; // [A1, A2, A3, A4, B1, B2, B3, B4, Ax = A1,A2,A3,A4, Bx = B1,B2,B3,B4, Cx = Ax,Bx]

// Single value setup
unsigned int control_lamps_values[2] = {0}; // [0% - 100%]
unsigned int control_water_amount[8] = {0}; // [0 ml/h - (max / amount) ml/h]

// Schedule setup, a maximum of 24 hours
// When the schedule ends it starts again at 0
// Uses the control arrays to figure out which to control, so {A, B} {{10, 0}} will turn A to 10% on 1.
int schedule_lamps[25][2] = {{0}, {-1}}; // End signal -1 as last item
int schedule_water_amount[25][8] = {{0}, {-1}};

int t_begin = -2; // Hour to begin schedule at index 0, -1 if start immediately
int t_end_days = -1; // End after x days, -1 to never end

bool schedule_started = false;


// === Functions ===
// == Pure functions ==
// Returns array of bits expressing a number (0 to 7)
bool* int_to_bin(int num, bool* bits) {
  bits[0] = num & 0b001; // 0bxx1 (AND operator)
  bits[1] = num & 0b010; // 0bx1x
  bits[2] = num & 0b100; // 0b1xx
  return bits; // Insert objects into the memory location
}

// Split a string
// https://stackoverflow.com/questions/9072320/split-string-into-string-array
String getValue(String data, char separator, int index) {
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = data.length() - 1;

  for (int i = 0; i <= maxIndex && found <= index; i++) {
    if (data.charAt(i) == separator || i == maxIndex) {
        found++;
        strIndex[0] = strIndex[1] + 1;
        strIndex[1] = (i == maxIndex) ? i + 1 : i;
    }
  }

  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

// == Control functions ==
// Turns on the lights at a given intensity
void io_lamps(int* pins, int value) {
  float percentage = (float) value / 100; // [0-1]
  int lamp_count = round((percentage * (lamp_amount / 2))); // [0-7]

  bool bits[lamp_pins];
  bool* control = int_to_bin(lamp_count, bits); // Integer expressed in binary, turn on different pins
  for (int i = 0; i < lamp_pins; i += 1) {
    digitalWrite(pins[i], !control[i]);
  }
}

// Turns on the pump
void io_pump() {
  if (pump_type == 'S') { // Stepper motor
    int stp = 1; // Lowest time = maximum speed

    // Switch signal to stepper motor, async delay
    if (millis() - t_pump > (2 * stp)) {
      digitalWrite(pump_step, false);
      t_pump = millis();
    } else if (millis() - t_pump > stp) {
      digitalWrite(pump_step, true);
    }
  } else if (pump_type == 'D') { // DC-motor
    digitalWrite(pump_step, false);
  }
}

// Opens a valve
void io_valve(int num) {
  bool bits[valve_bits];
  bool* control = int_to_bin(num, bits); // Integer expressed in binary, turn on different pins
  for (int i = 0; i < valve_bits; i += 1) {
    digitalWrite(valve[i], control[i]);
  }
}

// Selects which valve to turn on based on the time passed
void io_valves_select(int* nums, float* factors, int amount) {
  if (amount > 0) {
    unsigned long duration = float (switch_rate) / amount; // Max time spent per valve in the switch rate time

     if (duration > 0) {
      if (millis() - t_valve >= switch_rate) { // Entire timeframe (hour) has to be over before resetting
        t_valve = millis(); // Reset reference
      }

      // Check if it is time for a valve (n, n-1, n-2, ..., 0)
      int last = amount - 1;
      for (int i = last; i >= 0; i--) {
        // Calculate time when this valve opens
        unsigned long valve_start = 0;
        for (int j = 0; j < i; j++) {
          valve_start += duration * factors[j]; // Using the settings to determine the duration of water
        }

        // Check if time over
        if (i == last) {
          if ((millis() - t_valve) > (valve_start + (duration * factors[last]))) { // Time for the last valve over
             if (pump_type == 'D') {
              digitalWrite(pump_step, true); // Manually off
            }
            break; // End, pauze until next iteration
          }
        }

        // Control a certain valve
        if (millis() - t_valve > valve_start) { // Async delay
          io_valve(nums[i]);
          io_pump();
          break;
        }
      }
    }
  }
}

// == Other functions ==
// Update the time tracking values
void timer() {
  // Time since start
  if (millis() - t_prev >= (60000)) { // Accounts for overflow of millis function
    t_minutes += 1;
    t_hours = t_minutes / 60; // Rounded down (int datatype)
    t_days = t_hours / 24;
    t_prev = millis(); // Reset reference

    // Time of uploading via timestamp
    t_start_hour = getValue(__TIME__, ':', 0).toInt();
    t_start_minute = getValue(__TIME__, ':', 1).toInt();
  
    // Real time
    unsigned long r_minutes = t_minutes + t_start_minute;
    t_real_minute = r_minutes % 60;
    t_real_hour = (t_start_hour + (r_minutes / 60)) % 24;

    Serial.print(F(" - Dag "));
    Serial.print(t_days);
    Serial.print(F(" actief, tijd is "));
    Serial.print(t_real_hour);
    Serial.print(':');
    Serial.println(t_real_minute);
  }
}

// Collect data from sensors
void collect() {
  unsigned int temp_analog = 1024 - analogRead(temp_pin);
  temp[data_point] = temp_low + ((temp_high - temp_low) * temp_analog / 1024.0);
  
  unsigned int air_humid_analog = analogRead(air_humid_pin);
  air_humid[data_point] = air_humid_low + ((air_humid_high - air_humid_low) * air_humid_analog / 1024.0); 

  for (int i = 0; i < humid_pins; i++) {
    unsigned int humid_analog = analogRead(humid[i]);
    double frac = 1 - temp_analog / 1024.0;
    gnd_humidity[i][data_point] = 100 * frac / 0.65; // Make water 100%
  }
  
  data_point += 1;
  data_point %= data_points;
}


// === Setup initial state ===
void setup() {
  Serial.begin(9600);

  // Set pinmodes and start values
  for (int i = 0; i < lamp_pins; i += 1) {
    pinMode(lampA[i], OUTPUT);
    pinMode(lampB[i], OUTPUT);
    digitalWrite(lampA[i], true);
    digitalWrite(lampB[i], true);
  }
  for (int i = 0; i < valve_bits; i += 1) {
    pinMode(valve[i], OUTPUT);
    digitalWrite(valve[i], true);
  }
  pinMode(pump_dir, OUTPUT);
  pinMode(pump_step, OUTPUT);

  digitalWrite(pump_dir, true); // Don't change direction
  digitalWrite(pump_step, true); // Start off

  schedule_lamps[24][0] = -1;
  schedule_water_amount[24][0] = -1;

  if (pump_type == 'D') { // DC-motor
    pump_max_flowrate = 3404; // ml/hour
  } else { // Stepper motor
    pump_max_flowrate = 124; // ml/hour TODO, calibrate
  }
}


// === Control flow ===
void loop() {
  // == Main ==
  timer();

  if (data_col) {
    collect();
  }
  
  if (!control_fixed && t_end_days != -1) {
    // Check if still running
    if (t_days > t_end_days) {
      return; // Stop executing further
    } else if (t_end_days == t_days) {
      if (t_begin == -1) {
        return;
      } else if (t_real_hour >= t_begin) {
        return;
      }
    }
  }    
  
  // Wait until start time
  if (!schedule_started && !control_fixed) {
    if (t_begin != -1) {
      if (t_real_hour == t_begin) {
        schedule_started = true;
        t_valve = millis();
      }
    } else {
      schedule_started = true;
      t_valve = millis();
    }
  }

  if (schedule_started || control_fixed) { // Run control flow
    int schedule_index;
    if (t_begin == -1) {
      schedule_index = t_hours % 24;
    } else {
      schedule_index = ((int) t_real_hour - (int) t_begin + 24) % 24; // Hour of the day
    }

    // Lights control
    if (true) {
      // Point in the schedule
      int* control;
      if (control_fixed) { // Fixed value
         control = control_lamps_values;
      } else { // Schedule
        // Count number of hours in schedule
        int len = 0;
        for (int i = 0; i <= 24; i++) {
          if (schedule_lamps[i][0] == -1) { // End signal
            break;
          } else {
            len += 1;
          }
        }
        int at = schedule_index % len; // Place in schedule, repeat after ended
        control = schedule_lamps[at];
      }

      // Turn light on and off
      for (int i = 0; i < 2; i++) {
        if (control_lamps[i] == 'C') {
          io_lamps(lampA, control[i]);
          io_lamps(lampB, control[i]);
        }
        if (control_lamps[i] == 'A') {
          io_lamps(lampA, control[i]);

        }
        if (control_lamps[i] == 'B') {
          io_lamps(lampB, control[i]);
        }
      }
    }

    // Water
    if (true) {
      // Point in schedule
      int* control;
      if (control_fixed) {
         control = control_water_amount;
      } else { // Schedule
        // Count number of hours in schedule
        int len = 0;
        for (int i = 0; i <= 24; i++) {
          if (schedule_water_amount[i][0] == -1) { // End signal
            break;
          } else {
            len += 1;
          }
        }

        int at = schedule_index % len;
        control = schedule_water_amount[at];
      }


      // Amount of valves to be opened
      int amount = 0;
      for (int i = 0; i < 8; i ++) {
        if (control_valves[i] == "Cx") {
          amount = 8;
          break;
        } else if (control_valves[i] == "Ax" || control_valves[i] == "Bx") {
          amount += 4;
        } else if (control_valves[i] != 0) { // Not empty array location
          amount += 1;
        }
      }


      // Which valves to control
      float max_value = pump_max_flowrate / amount; // Max possible per valve
      float values[amount];
      int valves[amount];
      int at = 0;
      for (int j = 0; j < 8; j ++) {
        if (control_valves[j] == "Cx") {
          for (int i = 0; i < valve_amount; i++) {
            valves[at] = i;
            values[at] = control[j] / max_value; // Group of valves open with same setting
            at += 1;
          }
        } else if (control_valves[j] == "Bx") {
          for (int i = 0; i < (valve_amount / 2); i++) {
            valves[at] = i;
            values[at] = control[j] / max_value;
            at += 1;
          }
        } else if (control_valves[j] == "Ax") {
          for (int i = (valve_amount / 2); i < valve_amount; i++) {
            valves[at] = i;
            values[at] = control[j] / max_value;
            at += 1;
          }
        } else {
          if (control_valves[j] != 0) { // Not an empty spot in the array
            if (control_valves[j] == "B1") {
              valves[at] = 0;
            } else if (control_valves[j] == "B2") {
              valves[at] = 1;
            } else if (control_valves[j] == "B3") {
              valves[at] = 2;
            } else if (control_valves[j] == "B4") {
              valves[at] = 3;
            } else if (control_valves[j] == "A1") {
              valves[at] = 4;
            } else if (control_valves[j] == "A2") {
              valves[at] = 5;
            } else if (control_valves[j] == "A3") {
              valves[at] = 6;
            } else if (control_valves[j] == "A4") {
              valves[at] = 7;
            }

            values[at] = control[j] / max_value;
            at += 1;
          }
        }
      }

      // Open valves
      io_valves_select(valves, values, amount);
    }
  }
}


// === Events ===
// Runs when data comes in
void serialEvent() {
  // Get and process user input
  String input = Serial.readStringUntil('\n');

  if (input.equalsIgnoreCase("CANCEL")) {
    inp_index = 0;
    Serial.println(F("Enkele settings zijn veranderd."));
  } else if (input.equalsIgnoreCase("RESET")) {
    control_fixed = true;
    data_col = false;
    t_begin = -1;
    t_end_days = -1;

    for (int i = 0; i < 2; i++) {
     control_lamps_values[i] = 0;
      for (int j = 0; j < 24; j++) {
        schedule_lamps[j][i] = 0;
      }
      control_lamps[i] = 0;
    }
    control_lamps[0] = 'C';
    for (int i = 0; i < 8; i++) {
     control_water_amount[i] = 0;
      for (int j = 0; j < 24; j++) {
        schedule_water_amount[j][i] = 0;
      }
      control_valves[i] = "";
    }
    control_valves[0] = "Cx";

    inp_index = 0;
    Serial.println(F("Alle settings zijn gereset."));
  }

  switch (inp_index) {
    case 0: {
      if (input.equalsIgnoreCase("INPUT")) { // Start signal
        Serial.println(F("Begonnen met aanpassen settings."));
        Serial.println(F("Typ CANCEL om te stoppen met input."));
        Serial.println(F("Typ RESET om alles uit te zetten."));
        Serial.println(F("Data verzamelen (aan/uit):"));
        inp_index += 1;
      }
      break;
    }
    case 1: { // Data collection
      if (input.equalsIgnoreCase("AAN")) {
        data_col = true;
      } else if (input.equalsIgnoreCase("UIT")) {
        data_col = false;
      } else {
        break; // Try again
      }

      Serial.println(input);
      Serial.println(F("Een schema voor de kas in plaats van een vaste waarde (aan/uit):"));
      inp_index += 1;
      break;
    }
    case 2: { // Schedule on/off
      if (input.equalsIgnoreCase("AAN")) {
        control_fixed = false;
        t_begin = -2; // Don't start until set
        t_end_days = -1;
        schedule_started = false;
      } else if (input.equalsIgnoreCase("UIT")) {
        control_fixed = true;
      } else {
        break; // Try again
      }

      Serial.println(input);
      Serial.println(F("Welke lampengroepen wil je aansturen:"));
      inp_index += 1;
      break;
    }
    case 3: { // Light groups
      // Komma-separated list
      for (int i = 0; i < 2; i++) {
        String val = getValue(input, ',', i);
        val.trim();

        char opt = val.charAt(0);
        if (opt == 'A' || opt == 'B' || opt == 'C') { // Legal
          control_lamps[i] = opt;
          Serial.println(val);
        } else if (!opt) { // Empty spot in list
          control_lamps[i] = 0;
        } else {
          Serial.println(val);
          Serial.println(F("Not a group name, try again"));
          return;
        }

        // Reset values
        control_lamps_values[i] = 0;
        for (int j = 0; j < 24; j++) {
          schedule_lamps[j][i] = 0;
        }
      }

      // Verschillende keuzepaden
      if (control_fixed) {
        Serial.println(F("Geef voor alle lampen(groepen) het percentage aan (formaat: 1, 2):"));
        inp_index += 1;
      } else {
        Serial.println(F("Geef voor alle lampen(groepen) een percentage aan per uur (formaat: 1, 2; 3, 4):"));
        inp_index += 2;
      }
      break;
    }
    case 4: { // Light fixed
      for (int i = 0; i < 2; i++) {
        String val = getValue(input, ',', i);
        val.trim();

        if (val.length() != 0) { // Not empty position
          int opt = val.toInt();
          if (opt >= 0 && opt <= 100) { // Legal
            control_lamps_values[i] = opt;
            Serial.println(opt);
          } else {
            Serial.println(opt);
            Serial.println(F("Onmogelijke waarde, probeer opnieuw."));
            return;
          }
        } else {
          control_lamps_values[i] = 0;
        }
      }
      Serial.println(F("Welke watergroepen wil je aansturen:"));
      inp_index += 2;
      break;
    }
     case 5: { // Light schedule
      for (int j = 0; j < 24; j++) {
        String sched_hour = getValue(input, ';', j);
        sched_hour.trim();

        if (sched_hour.length() != 0) { // Not empty spot
          String toPrint = "Tijdens uur ";
          toPrint.concat(j + 1);
          Serial.println(toPrint);

           for (int i = 0; i < 2; i++) {
            String val = getValue(sched_hour, ',', i);
            val.trim();

            if (val.length() != 0) { // Not empty position
              int opt = val.toInt();
              if (opt >= 0 && opt <= 100) { // Legal
                schedule_lamps[j][i] = opt;
                Serial.println(opt);
              } else {
                Serial.println(opt);
                Serial.println(F("Onmogelijke waarde, probeer opnieuw."));
                return;
              }
            }
          }
        } else { // Empty day
          schedule_lamps[j][0] = -1; // End signal for program
          break;
        }
      }
      Serial.println(F("Welke watergroepen wil je aansturen:"));
      inp_index += 1;
      break;
    }
     case 6: { // Water groups
      // Komma-separated list
      for (int i = 0; i < 8; i++) {
        String opt = getValue(input, ',', i);
        opt.trim();

        if (((opt.charAt(0) == 'A' || opt.charAt(0) == 'B') &&
            (opt.charAt(1) == 'x' || opt.charAt(1) == '1' || opt.charAt(1) == '2' || opt.charAt(1) == '3' || opt.charAt(1) == '4')) ||
            opt == "Cx") { // Legal
          control_valves[i] = opt;
          Serial.println(opt);
        } else if (opt.length() == 0) { // Empty spot in list
          control_valves[i] = "";
        } else {
          Serial.println(opt);
          Serial.println(F("Not a group name, try again"));
          return;
        }
        // Reset values
        control_water_amount[i] = 0;
        for (int j = 0; j < 24; j++) {
          schedule_water_amount[j][i] = 0;
        }
      }

      if (control_fixed) {
        Serial.println(F("Geef voor alle watergroepen de hoeveelheid in mL/uur aan (formaat: 1, 2):"));
        inp_index += 1;
      } else {
        Serial.println(F("Geef voor alle watergroepen de hoeveelheid in mL/uur aan voor elk uur (formaat: 1, 2; 3, 4):"));
        inp_index += 2;
      }
      Serial.print(F("Maximaal volume pomp: "));
      Serial.println(pump_max_flowrate);
      break;
    }
     case 7: { // Fixed water
      for (int i = 0; i < 8; i++) {
        String val = getValue(input, ',', i);
        val.trim();

        if (val.length() != 0) { // Not empty position
          int opt = val.toInt();
          if (opt >= 0) { // Legal
            control_water_amount[i] = opt;
            Serial.println(opt);
          } else {
            Serial.println(opt);
            Serial.println(F("Onmogelijke waarde, probeer opnieuw."));
            return;
          }
        } else {
          control_water_amount[i] = 0;
        }
      }
      t_valve = millis();
      Serial.println(F("De settings zijn veranderd."));
      inp_index = 0;
      break;
    }
     case 8: { // Schedule water
      for (int j = 0; j < 24; j++) {
        String sched_hour = getValue(input, ';', j);
        sched_hour.trim();

        if (sched_hour.length() != 0) { // Not empty spot
          String toPrint = "Tijdens uur ";
          toPrint.concat(j + 1);
          Serial.println(toPrint);

          for (int i = 0; i < 8; i++) {
            String val = getValue(sched_hour, ',', i);
            val.trim();

            if (val.length() != 0) { // Not empty position
              int opt = val.toInt();
              if (opt >= 0) { // Legal
                schedule_water_amount[j][i] = opt;
                Serial.println(opt);
              } else {
                Serial.println(opt);
                Serial.println(F("Onmogelijke waarde, probeer opnieuw."));
                return;
              }
            }
          }
        } else { // Empty day
          schedule_water_amount[j][0] = -1; // End signal for program
          break;
        }
      }
      Serial.println(F("Hoeveel dagen moet het schema lopen (ALTIJD voor geen enddatum)."));
      inp_index += 1;
      break;
    }
    case 9: { // End day
      if (input.equalsIgnoreCase("ALTIJD")) {
        t_end_days = -1;
      } else {
        int val = input.toInt();
        if (val > 0) { // Legal
          t_end_days = t_days + val; // End after this day
          Serial.println(val);
        } else {
          Serial.println(val);
          Serial.println(F("Onmogelijke waarde, probeer opnieuw."));
          return;
        }
      }
      Serial.println(F("Geef aan op welk tijdstip (1 - 24) je het schema wil laten beginnen (NU voor meteen beginnen)"));
      inp_index += 1;
      break;
    }
  case 10: { // Start time
      if (input.equalsIgnoreCase("NU")) {
        Serial.println(input);
        t_begin = -1;
      } else {
        int val = input.toInt();
        if (val > 0 && val <= 24) { // Legal
          t_begin = val;
          Serial.println(val);
        } else {
          Serial.println(val);
          Serial.println(F("Onmogelijke waarde, probeer opnieuw."));
          return;
        }
      }
      inp_index = 0;
      Serial.println(F("De settings zijn veranderd."));
      break;
    }
  }
}
