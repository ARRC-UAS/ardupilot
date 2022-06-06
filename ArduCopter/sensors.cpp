#include "Copter.h"

void Copter::init_CASS_imet(){

    float coeff[4][4];

    //CS3D SENSORS (dummy values)
    //IMET temp number 57560:
    coeff[0][0] = 9.98873354e-04f;
    coeff[0][1] = 2.63219974e-04f;
    coeff[0][2] = 0.0f;
    coeff[0][3] = 1.47120693e-07f;

    //IMET temp number 57551:
    coeff[1][0] = 1.02017189e-03f;
    coeff[1][1] = 2.60496203e-04f;
    coeff[1][2] = 0.0f;
    coeff[1][3] = 1.52569843e-07f;

    //IMET temp number 57558:
    coeff[2][0] = 1.01048989e-03f;
    coeff[2][1] = 2.62050421e-04f;
    coeff[2][2] = 0.0f;
    coeff[2][3] = 1.48891207e-07f;

    //IMET temp number none:
    coeff[3][0] = 1.0f;
    coeff[3][1] = 1.0f;
    coeff[3][2] = 0.0f;
    coeff[3][3] = 1.0f;
    
    // Initialize and set I2C addresses
    uint8_t deafult_i2cAddr = 0x48;
    uint8_t busId = 0;
    for(uint8_t i=0; i<4; i++){
        CASS_Imet[i].init(busId,deafult_i2cAddr + i);
    }
    // Set sensor coefficients
    CASS_Imet[0].set_sensor_coeff(coeff[0]);
    CASS_Imet[1].set_sensor_coeff(coeff[1]);
    CASS_Imet[2].set_sensor_coeff(coeff[2]);
    CASS_Imet[3].set_sensor_coeff(coeff[3]);
}

void Copter::init_CASS_hyt271(){
    // Initialize and set I2C addresses
    uint8_t deafult_i2cAddr = 0x10;
    uint8_t busId = 0;
    for(uint8_t i=0; i<4; i++){
        CASS_HYT271[i].init(busId,deafult_i2cAddr + i);
    }
}

void Copter::init_ARRC_lb5900(){
    // Initialize and set I2C addresses
    uint8_t i2cAddr = g2.user_parameters.get_lb5900_address();
    uint16_t freq = g2.user_parameters.get_lb5900_freq();
    uint8_t avg_cnt = g2.user_parameters.get_lb5900_avg_cnt();
    uint8_t busId = 0;
    ARRC_LB5900.init(busId,i2cAddr,freq,avg_cnt);
}

// return barometric altitude in centimeters
void Copter::read_barometer(void)
{
    barometer.update();

    baro_alt = barometer.get_altitude() * 100.0f;

    motors->set_air_density_ratio(barometer.get_air_density_ratio());
}

void Copter::init_rangefinder(void)
{
#if RANGEFINDER_ENABLED == ENABLED
   rangefinder.set_log_rfnd_bit(MASK_LOG_CTUN);
   rangefinder.init(ROTATION_PITCH_270);
   rangefinder_state.alt_cm_filt.set_cutoff_frequency(g2.rangefinder_filt);
   rangefinder_state.enabled = rangefinder.has_orientation(ROTATION_PITCH_270);

   // upward facing range finder
   rangefinder_up_state.alt_cm_filt.set_cutoff_frequency(g2.rangefinder_filt);
   rangefinder_up_state.enabled = rangefinder.has_orientation(ROTATION_PITCH_90);
#endif
}

// return rangefinder altitude in centimeters
void Copter::read_rangefinder(void)
{
#if RANGEFINDER_ENABLED == ENABLED
    rangefinder.update();

#if RANGEFINDER_TILT_CORRECTION == ENABLED
    const float tilt_correction = MAX(0.707f, ahrs.get_rotation_body_to_ned().c.z);
#else
    const float tilt_correction = 1.0f;
#endif

    // iterate through downward and upward facing lidar
    struct {
        RangeFinderState &state;
        enum Rotation orientation;
    } rngfnd[2] = {{rangefinder_state, ROTATION_PITCH_270}, {rangefinder_up_state, ROTATION_PITCH_90}};

    for (uint8_t i=0; i < ARRAY_SIZE(rngfnd); i++) {
        // local variables to make accessing simpler
        RangeFinderState &rf_state = rngfnd[i].state;
        enum Rotation rf_orient = rngfnd[i].orientation;

        // update health
        rf_state.alt_healthy = ((rangefinder.status_orient(rf_orient) == RangeFinder::Status::Good) &&
                                (rangefinder.range_valid_count_orient(rf_orient) >= RANGEFINDER_HEALTH_MAX));

        // tilt corrected but unfiltered, not glitch protected alt
        rf_state.alt_cm = tilt_correction * rangefinder.distance_cm_orient(rf_orient);

        // remember inertial alt to allow us to interpolate rangefinder
        rf_state.inertial_alt_cm = inertial_nav.get_altitude();

        // glitch handling.  rangefinder readings more than RANGEFINDER_GLITCH_ALT_CM from the last good reading
        // are considered a glitch and glitch_count becomes non-zero
        // glitches clear after RANGEFINDER_GLITCH_NUM_SAMPLES samples in a row.
        // glitch_cleared_ms is set so surface tracking (or other consumers) can trigger a target reset
        const int32_t glitch_cm = rf_state.alt_cm - rf_state.alt_cm_glitch_protected;
        if (glitch_cm >= RANGEFINDER_GLITCH_ALT_CM) {
            rf_state.glitch_count = MAX(rf_state.glitch_count+1, 1);
        } else if (glitch_cm <= -RANGEFINDER_GLITCH_ALT_CM) {
            rf_state.glitch_count = MIN(rf_state.glitch_count-1, -1);
        } else {
            rf_state.glitch_count = 0;
            rf_state.alt_cm_glitch_protected = rf_state.alt_cm;
        }
        if (abs(rf_state.glitch_count) >= RANGEFINDER_GLITCH_NUM_SAMPLES) {
            // clear glitch and record time so consumers (i.e. surface tracking) can reset their target altitudes
            rf_state.glitch_count = 0;
            rf_state.alt_cm_glitch_protected = rf_state.alt_cm;
            rf_state.glitch_cleared_ms = AP_HAL::millis();
        }

        // filter rangefinder altitude
        uint32_t now = AP_HAL::millis();
        const bool timed_out = now - rf_state.last_healthy_ms > RANGEFINDER_TIMEOUT_MS;
        if (rf_state.alt_healthy) {
            if (timed_out) {
                // reset filter if we haven't used it within the last second
                rf_state.alt_cm_filt.reset(rf_state.alt_cm);
            } else {
                rf_state.alt_cm_filt.apply(rf_state.alt_cm, 0.05f);
            }
            rf_state.last_healthy_ms = now;
        }

        // send downward facing lidar altitude and health to the libraries that require it
        if (rf_orient == ROTATION_PITCH_270) {
            if (rangefinder_state.alt_healthy || timed_out) {
                wp_nav->set_rangefinder_alt(rangefinder_state.enabled, rangefinder_state.alt_healthy, rangefinder_state.alt_cm_filt.get());
#if MODE_CIRCLE_ENABLED
                circle_nav->set_rangefinder_alt(rangefinder_state.enabled && wp_nav->rangefinder_used(), rangefinder_state.alt_healthy, rangefinder_state.alt_cm_filt.get());
#endif
#if HAL_PROXIMITY_ENABLED
                g2.proximity.set_rangefinder_alt(rangefinder_state.enabled, rangefinder_state.alt_healthy, rangefinder_state.alt_cm_filt.get());
#endif
            }
        }
    }

#else
    // downward facing rangefinder
    rangefinder_state.enabled = false;
    rangefinder_state.alt_healthy = false;
    rangefinder_state.alt_cm = 0;

    // upward facing rangefinder
    rangefinder_up_state.enabled = false;
    rangefinder_up_state.alt_healthy = false;
    rangefinder_up_state.alt_cm = 0;
#endif
}

// return true if rangefinder_alt can be used
bool print_flag = true;

bool Copter::rangefinder_alt_ok() const
{
    Location curr_loc;
    int32_t curr_alt_cm;

    // Get altitude wrt home
    if(!copter.ahrs.get_position(curr_loc)){
        return (rangefinder_state.enabled && rangefinder_state.alt_healthy);
    }

    if(!curr_loc.get_alt_cm(Location::AltFrame::ABOVE_HOME, curr_alt_cm)){
        return (rangefinder_state.enabled && rangefinder_state.alt_healthy);
    }

    // Get humidity
    float avg_hum = 0;
    float N = 0;
    for(int8_t i = 0; i < 4; i++){
        if(copter.CASS_HYT271[i].healthy()){
            avg_hum += copter.CASS_HYT271[i].relative_humidity();
            N++;
        }
    }
    if(N > 0){
        avg_hum /= N;
    }
    else{
        avg_hum = 0;
    }

    if(g2.user_parameters.get_gpslidar_alt() < 1.0f && g2.user_parameters.get_gpslidar_hum() < 1.0f){
        return (rangefinder_state.enabled && rangefinder_state.alt_healthy);
    }
    else if(curr_alt_cm > g2.user_parameters.get_gpslidar_alt()*100.0f || avg_hum > g2.user_parameters.get_gpslidar_hum()){
        copter.rangefinder_state.alt_healthy = false;
        if (avg_hum > g2.user_parameters.get_gpslidar_hum() && print_flag == false){
            copter.gcs().send_text(MAV_SEVERITY_WARNING, "High humidity: Lidar disabled");
            print_flag = true;
        }
    }
    else{
        copter.rangefinder_state.alt_healthy = true;
        if (print_flag == true){
            copter.gcs().send_text(MAV_SEVERITY_INFO, "Lidar enabled");
            print_flag = false;
        }
    }

    return (rangefinder_state.enabled && rangefinder_state.alt_healthy);
}

// return true if rangefinder_alt can be used
bool Copter::rangefinder_up_ok() const
{
    return (rangefinder_up_state.enabled && rangefinder_up_state.alt_healthy);
}

/*
  get inertially interpolated rangefinder height. Inertial height is
  recorded whenever we update the rangefinder height, then we use the
  difference between the inertial height at that time and the current
  inertial height to give us interpolation of height from rangefinder
 */
bool Copter::get_rangefinder_height_interpolated_cm(int32_t& ret)
{
    if (!rangefinder_alt_ok()) {
        return false;
    }
    ret = rangefinder_state.alt_cm_filt.get();
    float inertial_alt_cm = inertial_nav.get_altitude();
    ret += inertial_alt_cm - rangefinder_state.inertial_alt_cm;
    return true;
}


/*
  update RPM sensors
 */
void Copter::rpm_update(void)
{
#if RPM_ENABLED == ENABLED
    rpm_sensor.update();
    if (rpm_sensor.enabled(0) || rpm_sensor.enabled(1)) {
        if (should_log(MASK_LOG_RCIN)) {
            logger.Write_RPM(rpm_sensor);
        }
    }
#endif
}

void Copter::compass_cal_update()
{
    compass.cal_update();

    if (hal.util->get_soft_armed()) {
        return;
    }

    static uint32_t compass_cal_stick_gesture_begin = 0;

    if (compass.is_calibrating()) {
        if (channel_yaw->get_control_in() < -4000 && channel_throttle->get_control_in() > 900) {
            compass.cancel_calibration_all();
        }
    } else {
        bool stick_gesture_detected = compass_cal_stick_gesture_begin != 0 && !motors->armed() && channel_yaw->get_control_in() > 4000 && channel_throttle->get_control_in() > 900;
        uint32_t tnow = millis();

        if (!stick_gesture_detected) {
            compass_cal_stick_gesture_begin = tnow;
        } else if (tnow-compass_cal_stick_gesture_begin > 1000*COMPASS_CAL_STICK_GESTURE_TIME) {
#ifdef CAL_ALWAYS_REBOOT
            compass.start_calibration_all(true,true,COMPASS_CAL_STICK_DELAY,true);
#else
            compass.start_calibration_all(true,true,COMPASS_CAL_STICK_DELAY,false);
#endif
        }
    }
}

void Copter::accel_cal_update()
{
    if (hal.util->get_soft_armed()) {
        return;
    }
    ins.acal_update();
    // check if new trim values, and set them
    float trim_roll, trim_pitch;
    if(ins.get_new_trim(trim_roll, trim_pitch)) {
        ahrs.set_trim(Vector3f(trim_roll, trim_pitch, 0));
    }

#ifdef CAL_ALWAYS_REBOOT
    if (ins.accel_cal_requires_reboot()) {
        hal.scheduler->delay(1000);
        hal.scheduler->reboot(false);
    }
#endif
}

// initialise proximity sensor
void Copter::init_proximity(void)
{
#if HAL_PROXIMITY_ENABLED
    g2.proximity.init();
#endif
}
