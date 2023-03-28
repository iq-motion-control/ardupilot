#include <AP_HAL/AP_HAL.h>
#include <AP_Math/AP_Math.h>
#include "AP_MotorsPulsing.h"
#include <GCS_MAVLink/GCS.h>
#include <SRV_Channel/SRV_Channel.h>
extern const AP_HAL::HAL& hal;

const AP_Param::GroupInfo AP_MotorsPulsing::var_info[] = {
    AP_NESTEDGROUPINFO(AP_MotorsMulticopter, 0),
    // @Param: YAW_DIR
    // @DisplayName: Motor normal or reverse
    // @Description: Used to change motor rotation directions without changing wires
    // @Values: 1:normal,-1:reverse
    // @User: Standard
    AP_GROUPINFO("YAW_DIR", 1, AP_MotorsPulsing, _yaw_dir, 1),

    // @Param: ROTOR_YAW_FF
    // @DisplayName: Rotor torque FF gain
    // @Description: Used to add a feed forward term to yaw that can compensate for rotor torque
    // @Range: ? ?
    // @Units: ?
    // @Increment: float
    // @User: Standard
    AP_GROUPINFO("ROTOR_YAW_FF", 2, AP_MotorsPulsing, _rotor_yaw_ff, 0),
    
    // @Param: GYRO_FF
    // @DisplayName: Rotor gyroscopic FF gain
    // @Description: Used to add a feed forward term to compensate for the rotor's gyroscopic torque
    // @Range: ? ?
    // @Units: ?
    // @Increment: float
    // @User: Standard
    AP_GROUPINFO("GYRO_FF", 3, AP_MotorsPulsing, _gyro_ff_gain, 0),

    AP_GROUPEND
};
// init
void AP_MotorsPulsing::init(motor_frame_class frame_class, motor_frame_type frame_type)
{
    
    // make sure 4 output channels are mapped
    for (uint8_t i = 0; i < 4; i++) {
        add_motor_num(CH_1 + i);
    }


    // setup actuator scaling
    SRV_Channels::set_angle(SRV_Channels::get_motor_function(1), AP_MOTORS_COAX_SERVO_INPUT_RANGE);
    SRV_Channels::set_angle(SRV_Channels::get_motor_function(2), AP_MOTORS_COAX_SERVO_INPUT_RANGE);

    motor_enabled[AP_MOTORS_MOT_1] = true;
    motor_enabled[AP_MOTORS_MOT_4] = true;


    _mav_type = MAV_TYPE_QUADROTOR;

    // record successful initialisation if what we setup was the desired frame_class
    // GCS_SEND_TEXT(MAV_SEVERITY_NOTICE, "Frame");
    set_initialised_ok(frame_class == MOTOR_FRAME_PULSING);
}

// set frame class (i.e. quad, hexa, heli) and type (i.e. x, plus)
void AP_MotorsPulsing::set_frame_class_and_type(motor_frame_class frame_class, motor_frame_type frame_type)
{
    set_initialised_ok(frame_class == MOTOR_FRAME_PULSING);
}

// set update rate to motors - a value in hertz
void AP_MotorsPulsing::set_update_rate(uint16_t speed_hz)
{
    // record requested speed
    _speed_hz = speed_hz;

    uint32_t mask =
        1U << AP_MOTORS_MOT_5 |
        1U << AP_MOTORS_MOT_6 ;
    rc_set_freq(mask, _speed_hz);
}

void AP_MotorsPulsing::output_to_motors()
{
    switch (_spool_state) {
        case SpoolState::SHUT_DOWN:
            // sends minimum values out to the motors
            rc_write(AP_MOTORS_MOT_1, output_to_pwm(0)); // rotor
            rc_write(AP_MOTORS_MOT_4, output_to_pwm(0)); // tail
            rc_write_angle(AP_MOTORS_MOT_2, 0); // pitch
            rc_write_angle(AP_MOTORS_MOT_3, 0); // roll
            break;
        case SpoolState::GROUND_IDLE:
            // sends output to motors when armed but not flying
            rc_write_angle(AP_MOTORS_MOT_2, 0);
            rc_write_angle(AP_MOTORS_MOT_3, 0);
            set_actuator_with_slew(_actuator[AP_MOTORS_MOT_1], actuator_spin_up_to_ground_idle()); // spin up motors
            set_actuator_with_slew(_actuator[AP_MOTORS_MOT_4], actuator_spin_up_to_ground_idle());
            rc_write(AP_MOTORS_MOT_1, output_to_pwm(_actuator[AP_MOTORS_MOT_1]));
            rc_write(AP_MOTORS_MOT_4, output_to_pwm(_actuator[AP_MOTORS_MOT_4]));
            break;
        case SpoolState::SPOOLING_UP:
        case SpoolState::THROTTLE_UNLIMITED:
        case SpoolState::SPOOLING_DOWN:
            // set motor output based on thrust requests
            rc_write_angle(AP_MOTORS_MOT_2, _pitch_action * AP_MOTORS_COAX_SERVO_INPUT_RANGE); // pitch
            rc_write_angle(AP_MOTORS_MOT_3, _roll_action * AP_MOTORS_COAX_SERVO_INPUT_RANGE); // roll
            set_actuator_with_slew(_actuator[AP_MOTORS_MOT_1], thrust_to_actuator(_rotor_thrust));
            set_actuator_with_slew(_actuator[AP_MOTORS_MOT_4], thrust_to_actuator(_tail_thrust));
            rc_write(AP_MOTORS_MOT_1, output_to_pwm(_actuator[AP_MOTORS_MOT_1]));
            rc_write(AP_MOTORS_MOT_4, output_to_pwm(_actuator[AP_MOTORS_MOT_4]));
            break;
    }
    
}

// get_motor_mask - returns a bitmask of which outputs are being used for motors or servos (1 means being used)
//  this can be used to ensure other pwm outputs (i.e. for servos) do not conflict
uint32_t AP_MotorsPulsing::get_motor_mask()
{
    uint32_t motor_mask =
        1U << AP_MOTORS_MOT_1 |
        1U << AP_MOTORS_MOT_4;
    uint32_t mask = motor_mask_to_srv_channel_mask(motor_mask);

    // add parent's mask
    mask |= AP_MotorsMulticopter::get_motor_mask();

    return mask;
}

void AP_MotorsPulsing::output_armed_stabilizing()
{
    float   roll_thrust;                // roll thrust input value, +/- 1.0
    float   pitch_thrust;               // pitch thrust input value, +/- 1.0
    float   yaw_thrust;                 // yaw thrust input value, +/- 1.0
    float   throttle_thrust;            // throttle thrust input value, 0.0 - 1.0
    float   throttle_avg_max;           // throttle thrust average maximum value, 0.0 - 1.0
    float   rp_scale = 1.0f;           // this is used to scale the roll, pitch and yaw to fit within the motor limits

    // gyro ff and yaw ff. Also need rotor height above COM
    // float yaw_ff_gain = g2.user_parameters.get_yaw_ff_gain();
    // float gyro_ff = g2.user_parameters.get_gyro_ff_gain();
    Vector3f gyro_latest = ahrs.get_gyro_latest();
    // float yaw_ff = yaw_ff_gain * velocity_0 * velocity_0;
    // Vector3f blade_omega(gyro_latest.x, gyro_latest.y, gyro_latest.z-velocity_0);

    // Vector3f omega_cross = gyro_latest%blade_omega;


    // float x_roll = (roll + omega_cross.x * gyro_ff) * max_pulse_volts;
    // float y_pitch = (pitch + omega_cross.y * gyro_ff) * max_pulse_volts;
    // float yaw_des = (yaw + yaw_ff) * g2.user_parameters.get_yaw_dir();
    // float z_yaw = MAX(MIN(yaw_des * (max_tail-idle_tail) + idle_tail, max_tail), idle_tail);

    // apply voltage and air pressure compensation
    const float compensation_gain = get_compensation_gain();
    roll_thrust = (_roll_in + _roll_in_ff) * compensation_gain;
    pitch_thrust = (_pitch_in + _pitch_in_ff) * compensation_gain;
    yaw_thrust = (_yaw_in + _yaw_in_ff) * compensation_gain;
    throttle_thrust = get_throttle() * compensation_gain;
    throttle_avg_max = _throttle_avg_max * compensation_gain;


    // sanity check throttle is above zero and below current limited throttle
    if (throttle_thrust <= 0.0f) {
        throttle_thrust = 0.0f;
        limit.throttle_lower = true;
    }
    if (throttle_thrust >= _throttle_thrust_max) {
        throttle_thrust = _throttle_thrust_max;
        limit.throttle_upper = true;
    }

    throttle_avg_max = constrain_float(throttle_avg_max, throttle_thrust, _throttle_thrust_max);

    float rp_thrust_max = MAX(fabsf(roll_thrust), fabsf(pitch_thrust));

    // calculate how much roll and pitch must be scaled to leave enough range for the minimum yaw
    if (rp_thrust_max >= 1.0f) {
        rp_scale = constrain_float(1.0f / rp_thrust_max, 0.0f, 1.0f);
        if (rp_scale < 1.0f) {
            limit.roll = true;
            limit.pitch = true;
        }
    }

    if (fabsf(yaw_thrust) > 1.0f) {
        yaw_thrust = constrain_float(1.0f / yaw_thrust, -1.0f, 1.0f);
        limit.yaw = true;
    }


    // calculate the throttle setting for the lift fan
    // compensation_gain can never be zero
    _throttle_out = throttle_avg_max / compensation_gain;

    _roll_action = roll_thrust * rp_scale;
    _pitch_action = pitch_thrust * rp_scale;

    _rotor_thrust = _throttle_out;
    _tail_thrust = _yaw_dir * yaw_thrust;
}


// output_test_seq - spin a motor at the pwm value specified
//  motor_seq is the motor's sequence number from 1 to the number of motors on the frame
//  pwm value is an actual pwm value that will be output, normally in the range of 1000 ~ 2000
void AP_MotorsPulsing::_output_test_seq(uint8_t motor_seq, int16_t pwm)
{
    // output to motors and servos
    switch (motor_seq) {
        case 1:
            // flap servo 1
            rc_write(AP_MOTORS_MOT_1, pwm);
            break;
        case 2:
            // flap servo 2
            rc_write(AP_MOTORS_MOT_2, pwm);
            break;
        case 3:
            // flap servo 3
            rc_write(AP_MOTORS_MOT_3, pwm);
            break;
        case 4:
            // flap servo 4
            rc_write(AP_MOTORS_MOT_4, pwm);
            break;
        default:
            // do nothing
            break;
    }
}