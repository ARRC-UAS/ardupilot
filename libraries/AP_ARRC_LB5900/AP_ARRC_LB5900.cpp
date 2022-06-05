#include "AP_ARRC_LB5900.h"
#include <utility>
#include <stdio.h>
#include <string.h>
#include <AP_HAL/AP_HAL.h>
#include <AP_Math/AP_Math.h>

extern const AP_HAL::HAL &hal;

AP_ARRC_LB5900::AP_ARRC_LB5900() :
    _dev(nullptr),
    _power(0),
    _healthy(false)
{
}

bool AP_ARRC_LB5900::init(uint8_t busId, uint8_t i2cAddr, uint16_t freq, uint8_t avg_cnt)
{
    commandNumber = 0;
    Sensor_TimeOut = 200;

    // Bus 0 is for Pixhawk 2.1 I2C and Bus 1 is for Pixhawk 1 and PixRacer I2C
    // Check if device exists
    _dev = std::move(hal.i2c_mgr->get_device(busId, i2cAddr));
    if (!_dev) {
        _healthy = false;
        return false;
    }
    _healthy = true;

    hal.scheduler->delay(500);
    
    _dev->get_semaphore()->take_blocking();

    _dev->set_retries(2);

    // Start the first measurement
    uint16_t iter = 0;
    while(!configSensor(freq, avg_cnt)) {
        hal.scheduler->delay(5);
        if (iter == 100){
            _healthy = false;
            _dev->get_semaphore()->give();
            return false;
        }
        iter++;
    }

    _dev->get_semaphore()->give();

    _dev->register_periodic_callback(1000000, FUNCTOR_BIND_MEMBER(&AP_ARRC_LB5900::_timer, void));

    return true;
}

void AP_ARRC_LB5900::set_i2c_addr(uint8_t addr)
{
    if (_dev) {
        _dev->set_address(addr);
    }
}


bool AP_ARRC_LB5900::configSensor(uint16_t freq, uint8_t avg_cnt)
{
    char FREQ[10 + sizeof(char)] = "FREQ ";
    char AVG_CNT[12 + sizeof(char)] = "AVER:COUN ";
    char temp[5 + sizeof(char)];

    // Convert user params freq and avg_cnt to strings
    snprintf(temp,6,"%d",freq);
    strcat(FREQ, temp);
    strcat(FREQ, " MHZ");
    snprintf(temp,6,"%d",avg_cnt);
    strcat(AVG_CNT, temp);

    // List of initial commands to configure the LB5900
    const char* (cmd[1])[10] = 
    {
        "SYST:PRES DEF",
        FREQ,
        "AVER:COUN:AUTO 0",
        AVG_CNT,
        "AVER:SDET 0",
        //"INIT:CONT 1",
        "\0" // STOP LIST
    };

    // Send initial commands through I2C
    while(1){
        if(strlen(cmd[0][commandNumber])  != 0 ){

            hal.scheduler->delay(2);
            // Build header
            memset(write_sensor_buffer.byte, 0x00, 50);
            write_sensor_buffer.field.commandAndLength[0] = (uint8_t)nextReadIsStatusAndLength;
            header.ui = strlen(cmd[0][commandNumber]) + 5; // Add one for terminator and 4 for header
            write_sensor_buffer.field.commandAndLength[3] = header.c[0];
            write_sensor_buffer.field.commandAndLength[2] = header.c[1];
            write_sensor_buffer.field.commandAndLength[1] = header.c[2];
            // Add command to buffer
            strcpy((char*)write_sensor_buffer.field.buffer, cmd[0][commandNumber]);
            // Send Command with "nextReadIsStatusAndLength" header
            //uint16_t iter = 0;
            if(!_dev->transfer(write_sensor_buffer.byte, header.ui, nullptr, 0)) {
                return false;
            }
            commandNumber++;
        }
        else{
            commandNumber = 0;
            hal.scheduler->delay(5);
            return true;
        }
    }
}

bool AP_ARRC_LB5900::_read(void)
{   
    // Clear headers
    bufLength.ui = 0;
    header.ui = 0;

    // Send Prepare Status & Length (no command) using header O6h
    memset(write_sensor_buffer.byte, 0x00, 50);
    write_sensor_buffer.field.commandAndLength[0] = (uint8_t)nextReadIsStatusAndLength;
    header.ui = 4; // Only 4 bytes are sent (without command)
    write_sensor_buffer.field.commandAndLength[3] = header.c[0];
    write_sensor_buffer.field.commandAndLength[2] = header.c[1];
    write_sensor_buffer.field.commandAndLength[1] = header.c[2];
    
    if(!_dev->transfer(write_sensor_buffer.byte, header.ui, nullptr, 0)) {
        return false;
    }
    hal.scheduler->delay(3);

    // Read 4 bytes from the sensor. This contains the status byte and 3 length bytes.
    // header.c is used as temp variable
    uint8_t iter = 0;
    fail:
        header.ui = 0; // Clear header
        if(!_dev->transfer(nullptr, 0, header.c, 4)) {
            return false;
        }

        // Transfer status&length to read_buffer
        bufLength.c[0] = header.c[3];
        bufLength.c[1] = header.c[2];
        bufLength.c[2] = header.c[1];

        // If status bit is good, get the data!
        if(header.c[0] && 0x10 == 1) // bufLength.ui != 0
        {
            _power = 100;
            // Write the number of bytes to the sensor that are to be read back using header 0Ch
            memset(write_sensor_buffer.byte, 0x00, 50);
            write_sensor_buffer.field.commandAndLength[0] = (uint8_t)nextReadIsCompleteOutputBuffer;
            write_sensor_buffer.field.commandAndLength[1] = bufLength.c[2]; // 0;
            write_sensor_buffer.field.commandAndLength[2] = bufLength.c[1]; // 0;
            write_sensor_buffer.field.commandAndLength[3] = bufLength.c[0]; // 4;

            hal.scheduler->delay(1);

            if(!_dev->transfer(write_sensor_buffer.byte, 4, nullptr, 0)) {
                return false;
            }
            hal.scheduler->delay(3);

            _power = 101;

            // Read back the measurement
            memset(read_sensor_buffer.byte, 0x00, 50);
            if(!_dev->transfer(nullptr, 0, read_sensor_buffer.byte, bufLength.ui)) {
                return false;
            }

            _power = 102;

            // Convert received bytes to number
            _power = strtof(read_sensor_buffer.string, nullptr);

            return true;
        }
        else if(iter < 1){
            hal.scheduler->delay(50);
            iter++;
            goto fail;
        }
        else{
            _power = 69;
            return false;
        }
}

bool AP_ARRC_LB5900::_measure(void)
{
    const char* (cmd[1])[10] = 
    {
        //"FETCH?",
        "READ?",
        "\0" // STOP LIST
    };

    while(1){
        if(strlen(cmd[0][commandNumber])  != 0 ){
            // Build header
            memset(write_sensor_buffer.byte, 0x00, 50);
            write_sensor_buffer.field.commandAndLength[0] = (uint8_t)nextReadIsStatusAndLength;
            header.ui = strlen(cmd[0][commandNumber]) + 5; // Add one for terminator and 4 for header
            write_sensor_buffer.field.commandAndLength[3] = header.c[0];
            write_sensor_buffer.field.commandAndLength[2] = header.c[1];
            write_sensor_buffer.field.commandAndLength[1] = header.c[2];
            // Add command to buffer
            strcpy((char*)write_sensor_buffer.field.buffer, cmd[0][commandNumber]);
            // Send Command with "nextReadIsStatusAndLength" header
            if(!_dev->transfer(write_sensor_buffer.byte, header.ui, nullptr, 0)) {
                commandNumber = 0;
                return false;
            }
            commandNumber++;
        }
        else{
            commandNumber = 0;
            return true;
        }
    }
}

void AP_ARRC_LB5900::_timer(void)
{
    WITH_SEMAPHORE(_sem);
    _healthy = _read();         // Read previous measurement request
    hal.scheduler->delay(3);
    _healthy &= _measure();      // Request a new measurement to the sensor
}