/*
    TrinamicDriverClass.cpp
    This is used for Trinamic SPI controlled stepper motor drivers.

    Part of Grbl_ESP32
    2020 -	Bart Dring
    
    Grbl is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    Grbl is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with Grbl.  If not, see <http://www.gnu.org/licenses/>.

*/
#include <TMCStepper.h>
#include "TrinamicDriverClass.h"

TrinamicDriver :: TrinamicDriver(uint8_t axis_index,
                                 uint8_t step_pin,
                                 uint8_t dir_pin,
                                 uint8_t disable_pin,
                                 uint8_t cs_pin,
                                 uint16_t driver_part_number,
                                 float r_sense,
                                 int8_t spi_index) {
    type_id = TRINAMIC_SPI_MOTOR;
    this->axis_index = axis_index % MAX_AXES;
    this->dual_axis_index = axis_index < 6 ? 0 : 1; // 0 = primary 1 = ganged
    _driver_part_number = driver_part_number;
    _r_sense = r_sense;
    this->step_pin = step_pin;
    this->dir_pin  = dir_pin;
    this->disable_pin = disable_pin;
    this->cs_pin = cs_pin;
    this->spi_index = spi_index;

    if (_driver_part_number == 2130)
        tmcstepper = new TMC2130Stepper(cs_pin, _r_sense, spi_index);
    else if (_driver_part_number == 5160)
        tmcstepper = new TMC5160Stepper(cs_pin, _r_sense, spi_index);
    else {
        grbl_msg_sendf(CLIENT_SERIAL, MSG_LEVEL_INFO, "Trinamic unsupported p/n:%d", _driver_part_number);
        return;
    }

    set_axis_name();

    init_step_dir_pins(); // from StandardStepper

    digitalWrite(cs_pin, HIGH);
    pinMode(cs_pin, OUTPUT);

    // use slower speed if I2S
    if (cs_pin >= I2S_OUT_PIN_BASE)
        tmcstepper->setSPISpeed(TRINAMIC_SPI_FREQ);

    config_message();

    // init() must be called later, after all TMC drivers have CS pins setup.
}

void TrinamicDriver :: init() {

    SPI.begin();  // this will get called for each motor, but does not seem to hurt anything

    tmcstepper->begin();
    test(); // Try communicating with motor. Prints an error if there is a problem.
    read_settings(); // pull info from settings
    set_mode();

    _is_homing = false;
    is_active = true;  // as opposed to NullMotors, this is a real motor
}

/*
    This is the startup message showing the basic definition
*/
void TrinamicDriver :: config_message() {
    grbl_msg_sendf(CLIENT_SERIAL,
                   MSG_LEVEL_INFO,
                   "%s Axis Trinamic TMC%d Step:%s Dir:%s CS:%s Disable:%s Index:%d",
                   _axis_name,
                   _driver_part_number,
                   pinName(step_pin).c_str(),
                   pinName(dir_pin).c_str(),
                   pinName(cs_pin).c_str(),
                   pinName(disable_pin).c_str(),
                   spi_index);
}

bool TrinamicDriver :: test() {
    char lib_ver[32];
    sprintf(lib_ver, "TMCStepper Ver 0x%06x", TMCSTEPPER_VERSION);

    switch (tmcstepper->test_connection()) {
    case 1:
        grbl_msg_sendf(CLIENT_SERIAL, MSG_LEVEL_INFO, "%s Trinamic driver test failed. Check connection. %s", _axis_name, lib_ver);
        return false;
    case 2:
        grbl_msg_sendf(CLIENT_SERIAL, MSG_LEVEL_INFO, "%s Trinamic driver test failed. Check motor power. %s", _axis_name, lib_ver);
        return false;
    default:
        grbl_msg_sendf(CLIENT_SERIAL, MSG_LEVEL_INFO, "%s Trinamic driver test passed. %s", _axis_name, lib_ver);
        return true;
    }
}


/*
    Read setting and send them to the driver. Called at init() and whenever related settings change
*/
void TrinamicDriver :: read_settings() {
    //grbl_msg_sendf(CLIENT_SERIAL, MSG_LEVEL_INFO, "%c Axis read_settings() ", report_get_axis_letter(axis_index));
    tmcstepper->microsteps(axis_settings[axis_index]->microsteps->get());
    tmcstepper->rms_current(axis_settings[axis_index]->run_current->get() * 1000.0, axis_settings[axis_index]->hold_current->get() / 100.0);
    tmcstepper->sgt(axis_settings[axis_index]->stallguard->get());
}

void TrinamicDriver :: set_homing_mode(bool is_homing) {
    _homing_mode = is_homing;
    set_mode();
}

/*
    There are ton of settings. I'll start by grouping then into modes for now.
    Many people will want quiet and stallgaurd homing. Stallguard only run in
    Coolstep mode, so it will need to switch to Coolstep when homing
*/
void TrinamicDriver :: set_mode() {


    if (_is_homing && (_homing_mode ==  TRINAMIC_HOMING_STALLGUARD))
        _mode = TRINAMIC_RUN_MODE_STALLGUARD;
    else {
        _mode = TRINAMIC_RUN_MODE;
                
}

    if (_mode == TRINAMIC_RUN_MODE_STEALTHCHOP) {
        //grbl_msg_sendf(CLIENT_SERIAL, MSG_LEVEL_INFO, "STEALTHCHOP");
        tmcstepper->toff(5);
        tmcstepper->en_pwm_mode(1);      // Enable extremely quiet stepping
        tmcstepper->pwm_autoscale(1);
    } else  {  // if (mode == TRINAMIC_RUN_MODE_COOLSTEP || mode == TRINAMIC_RUN_MODE_STALLGUARD)
        //grbl_msg_sendf(CLIENT_SERIAL, MSG_LEVEL_INFO, "COOLSTEP");
        tmcstepper->tbl(1);
        tmcstepper->toff(3);
        tmcstepper->hysteresis_start(4);
        tmcstepper->hysteresis_end(-2);
        tmcstepper->sfilt(1);
        tmcstepper->diag1_pushpull(0); // 0 = active low
        tmcstepper->diag1_stall(1); // stallguard i/o is on diag1
        if (_mode == TRINAMIC_RUN_MODE_COOLSTEP) {
            tmcstepper->TCOOLTHRS(NORMAL_TCOOLTHRS); // when to turn on coolstep
            tmcstepper->THIGH(NORMAL_THIGH);
        } else {
            uint32_t tcoolthrs = calc_tstep(homing_feed_rate->get(), 150.0);
            uint32_t thigh = calc_tstep(homing_feed_rate->get(), 60.0);
            //grbl_msg_sendf(CLIENT_SERIAL, MSG_LEVEL_INFO, "Tstep range %d - %d SGV:%d", thigh, tcoolthrs, tmcstepper->sgt());
            tmcstepper->TCOOLTHRS(tcoolthrs);
            tmcstepper->THIGH(thigh);
        }
    }
}

/*
    This is the stallguard tuning info. It is call debug, so it could be generic across all classes.
*/
void TrinamicDriver :: debug_message() {    

    uint32_t tstep = tmcstepper->TSTEP();

    if (tstep == 0xFFFFF || tstep == -1) {   // if axis is not moving return
        return;
    }     

    float feedrate = st_get_realtime_rate(); //* settings.microsteps[axis_index] / 60.0 ; // convert mm/min to Hz

    grbl_msg_sendf(CLIENT_SERIAL,
                   MSG_LEVEL_INFO,
                   "%s Stallguard %d   SG_Val: %04d   Rate: %05.0f mm/min SG_Setting:%d",
                   _axis_name,                   
                   tmcstepper->stallguard(),
                   tmcstepper->sg_result(),
                   feedrate,
                   axis_settings[axis_index]->stallguard->get());
}

// calculate a tstep from a rate
// tstep = TRINAMIC_FCLK / (time between 1/256 steps)
// This is used to set the stallguard window from the homing speed.
// The percent is the offset on the window
uint32_t TrinamicDriver :: calc_tstep(float speed, float percent) {
    float tstep = speed / 60.0 * axis_settings[axis_index]->steps_per_mm->get() * (float)(256 / axis_settings[axis_index]->microsteps->get());
    tstep = TRINAMIC_FCLK / tstep * percent / 100.0;

    return (uint32_t)tstep;
}


// this can use the enable feature over SPI. The dedicated pin must be in the enable mode,
// but that can be hardwired that way.
void TrinamicDriver :: set_disable(bool disable) {
    //grbl_msg_sendf(CLIENT_SERIAL, MSG_LEVEL_INFO, "%s Axis disable %d", _axis_name, disable);

    digitalWrite(disable_pin, disable);

#ifdef USE_TRINAMIC_ENABLE
    if (disable)
        tmcstepper->toff(0);
    else {
        set_mode(); // resets everything including toff
    }
#endif
    // the pin based enable could be added here.
    // This would be for individual motors, not the single pin for all motors.
}

