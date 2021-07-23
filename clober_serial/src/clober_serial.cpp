#include <clober_serial/clober_serial.hpp>

CloberSerial::CloberSerial()
    : port_("/dev/ttyUSB0"),
      baudrate_(115200),
      timeout_(50),
      control_frequency_(50.0),
      odom_frame_parent_("odom"),
      odom_frame_child_("base_link"),
      cmd_vel_timeout_(1.0),
      odom_mode_(0)
{
    nh_.getParam("port", port_);
    nh_.getParam("baud", baudrate_);
    nh_.getParam("timeout", timeout_);
    nh_.getParam("control_frequency", control_frequency_);
    nh_.getParam("odom_frame_parent", odom_frame_parent_);
    nh_.getParam("odom_frame_child", odom_frame_child_);
    nh_.getParam("cmd_vel_timeout", cmd_vel_timeout_);
    nh_.getParam("mode", odom_mode_);

    odom_freq_ = control_frequency_;

    SetValues();

    try
    {
        serial_ = std::make_unique<serial::Serial>(port_, baudrate_);
        serial::Timeout to = serial::Timeout::simpleTimeout(timeout_);
        serial_->setTimeout(to);
    }
    catch (const std::exception &e)
    {
        cout << "connection failed" << endl;
    }

    cout << "Connected to serial : " << port_ << " with baudrate " << baudrate_ << endl;

    odom_pub_ = nh_.advertise<nav_msgs::Odometry>("/odom", 1);

    cmd_vel_sub_ = nh_.subscribe<geometry_msgs::Twist>("/cmd_vel", 1, bind(&CloberSerial::cmd_vel_callback, this, _1));

    readThread_ = std::make_shared<thread>(bind(&CloberSerial::read_serial, this, 20));
    publishThread_ = std::make_shared<thread>(bind(&CloberSerial::publish_loop, this, control_frequency_));
}

CloberSerial::~CloberSerial()
{
    readThread_->detach();
    if (readThread_->joinable())
    {
        readThread_->join();
    }
}

void CloberSerial::SetValues()
{
    nh_.getParam("wheel_separation", config_.WIDTH);
    nh_.getParam("wheel_radius", config_.WheelRadius);
    nh_.getParam("wheel_max_speed_mps", config_.MAX_SPEED);
    nh_.getParam("wheel_max_rpm", config_.MAX_RPM);
    nh_.getParam("encoder_ppr", config_.encoder.ppr);

    config_.left_motor.position_rad = 0.0;
    config_.right_motor.position_rad = 0.0;
    trigger_ = false;
    posX_ = 0.0;
    posY_ = 0.0;
    heading_ = 0.0;
}

void CloberSerial::cmd_vel_callback(const geometry_msgs::Twist::ConstPtr &msg)
{
    motor_cmd_.linearVel = msg->linear.x;
    motor_cmd_.angularVel = msg->angular.z;
    cmd_vel_timeout_switch_ = false;

    cout << "cmd_vel callback" << endl;

    on_motor_move(motor_cmd_);
}

void CloberSerial::read_serial(int ms)
{
    while (ros::ok())
    {
        parse();
        // std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

void CloberSerial::parse()
{
    string msg = serial_->readline(max_line_length, eol);

    if (msg.size() > 2)
    {
        if (msg[0] == 'F' && msg[2] == ':')
        {
            try
            {
                size_t index = boost::lexical_cast<size_t>(msg[1]);

                vector<string> feedbacks;
                boost::split(feedbacks, msg, boost::algorithm::is_any_of(":"));

                if (feedbacks.size() > 8)
                {

                    config_.left_motor.rpm = boost::lexical_cast<float>(feedbacks[5]);
                    config_.left_motor.speed = utils_.toVelocity(boost::lexical_cast<float>(feedbacks[5]));

                    config_.right_motor.rpm = boost::lexical_cast<float>(feedbacks[6]);
                    config_.right_motor.speed = utils_.toVelocity(boost::lexical_cast<float>(feedbacks[6]));

                    config_.left_motor.position_rad += utils_.toRad(boost::lexical_cast<float>(feedbacks[7]), config_.encoder.ppr);
                    config_.left_motor.position_meter_curr = config_.left_motor.position_rad * config_.WheelRadius;

                    config_.right_motor.position_rad += utils_.toRad(boost::lexical_cast<float>(feedbacks[8]), config_.encoder.ppr);
                    config_.right_motor.position_meter_curr = config_.right_motor.position_rad * config_.WheelRadius;

                    float dl = config_.left_motor.position_meter_curr - config_.left_motor.position_meter_prev;
                    float dr = config_.right_motor.position_meter_curr - config_.right_motor.position_meter_prev;

                    // rad/s -> m/s
                    float l_speed = config_.left_motor.speed * config_.WheelRadius;
                    float r_speed = config_.right_motor.speed * config_.WheelRadius;

                    toVW(l_speed, r_speed);

                    switch (odom_mode_)
                    {
                    case 0:
                        updatePose();
                        break;
                    
                    case 1:
                        updatePose(l_speed, r_speed);
                        break;

                    case 2:
                        updatePose(dl, dr);
                        break;

                    default:
                        updatePose();
                        break;
                    }

                    config_.left_motor.position_meter_prev = config_.left_motor.position_meter_curr;
                    config_.right_motor.position_meter_prev = config_.right_motor.position_meter_curr;
                }
            }
            catch (boost::bad_lexical_cast &e)
            {
            }
        }
    }
}

float CloberSerial::toVW(float l_speed, float r_speed)
{
    linearVel_ = (l_speed + r_speed) / 2;
    angularVel_ = (r_speed - l_speed) / config_.WIDTH;
}

pair<float, float> CloberSerial::toWheelSpeed(float v, float w)
{
    // v, w -> wheel angular speed (rad/s)
    float r_speed = (v + (config_.WIDTH * w / 2)) / config_.WheelRadius;
    float l_speed = (v - (config_.WIDTH * w / 2)) / config_.WheelRadius;

    return make_pair(l_speed, r_speed);
}

float CloberSerial::limitMaxSpeed(float speed)
{
    // wheel angular max speed (rad/s)
    float max = config_.MAX_SPEED / config_.WheelRadius;
    if (abs(speed) > max)
    {
        if (speed > 0)
        {
            return max;
        }
        else
        {
            return -max;
        }
    }
    else
    {
        return speed;
    }
}



void CloberSerial::updatePose()
{
    ros::Time now = ros::Time::now();

    if (!trigger_)
    {
        timestamp_ = now;
        trigger_ = true;
        return;
    }

    double dT = now.toSec() - timestamp_.toSec();
    timestamp_ = now;

    float x = linearVel_ * dT * cos(heading_);
    float y = linearVel_ * dT * sin(heading_);
    float theta = angularVel_ * dT;

    posX_ += x;
    posY_ += y;
    heading_ += theta;

    // cout << "update pose x : " << posX_ << ", y : " << posY_ << endl;
}

void CloberSerial::updatePose(float dL, float dR)
{
    ros::Time now = ros::Time::now();

    if (!trigger_)
    {
        timestamp_ = now;
        trigger_ = true;
        return;
    }

    double dT = now.toSec() - timestamp_.toSec();
    timestamp_ = now;

    float x = posX_;
    float y = posY_;
    float theta = heading_;

    float R = 0.0;
    if ((dR - dL) < 0.0001)
    {
        R = 0.0;
    }
    else
    {
        R = (config_.WIDTH / 2.0) * ((dL + dR) / (dR - dL));
    }

    float Wdt;
    
    if ( odom_mode_ == 1 ){
        float W = (dR - dL) / config_.WIDTH;            // dR, dL 인자를 속도값으로 넘겼을 때, dT를 곱해서 계산
        Wdt = W*dT;
    }else if ( odom_mode_ == 2){
        Wdt = (dR - dL) / config_.WIDTH;                // dR, dL 인자를 거리값으로 넘겼을 때,
    }

    float ICCx = x - (R * sin(theta));
    float ICCy = y + (R * cos(theta));

    posX_ = (cos(Wdt) * (x - ICCx)) - (sin(Wdt) * (y - ICCy)) + ICCx;
    posY_ = (sin(Wdt) * (x - ICCx)) + (cos(Wdt) * (y - ICCy)) + ICCy;
    heading_ = theta + Wdt;

    // cout << "update pose x : " << posX_ << ", y : " << posY_ << endl;
}

void CloberSerial::publish_loop(int hz)
{
    int ms = 1000 * (1/hz);
    while (ros::ok())
    {
        publishOdom();
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

void CloberSerial::publishOdom()
{
    tf2::Quaternion q;
    q.setRPY(0.0, 0.0, heading_);

    nav_msgs::Odometry odom;
    odom.header.frame_id = odom_frame_parent_;
    odom.child_frame_id = odom_frame_child_;
    odom.header.stamp = timestamp_;
    odom.pose.pose.position.x = posX_;
    odom.pose.pose.position.y = posY_;
    odom.pose.pose.orientation.x = q.x();
    odom.pose.pose.orientation.y = q.y();
    odom.pose.pose.orientation.z = q.z();
    odom.pose.pose.orientation.w = q.w();
    odom.pose.covariance.fill(0.0);
    odom.pose.covariance[0] = 1e-3;
    odom.pose.covariance[7] = 1e-3;
    odom.pose.covariance[14] = 1e6;
    odom.pose.covariance[21] = 1e6;
    odom.pose.covariance[28] = 1e6;
    odom.pose.covariance[35] = 1e-3;

    odom.twist.twist.linear.x = linearVel_;
    odom.twist.twist.angular.z = angularVel_;
    odom.twist.covariance.fill(0.0);
    odom.twist.covariance[0] = 1e-3;
    odom.twist.covariance[7] = 1e-3;
    odom.twist.covariance[14] = 1e6;
    odom.twist.covariance[21] = 1e6;
    odom.twist.covariance[28] = 1e6;
    odom.twist.covariance[35] = 1e3;

    geometry_msgs::TransformStamped odom_tf;
    odom_tf.header.stamp = timestamp_;
    odom_tf.header.frame_id = odom_frame_parent_;
    odom_tf.child_frame_id = odom_frame_child_;
    odom_tf.transform.translation.x = posX_;
    odom_tf.transform.translation.y = posY_;
    odom_tf.transform.translation.z = 0;
    odom_tf.transform.rotation = odom.pose.pose.orientation;
    tf_broadcaster_.sendTransform(odom_tf);

    odom_pub_.publish(odom);

    // cout << "odom x : " << posX_ << ", y : " << posY_ << ", heading : " << heading_ << endl;

    // static int timeout_counter = 0;
    // if( !cmd_vel_timeout_switch_){
    //     timeout_counter = 0;
    //     cmd_vel_timeout_switch_ = true;
    // }else{
    //     if( timeout_counter > cmd_vel_timeout_*odom_freq_){
    //         motor_cmd_->linear.x = 0;
    //         motor_cmd_->angular.z = 0;
    //         timeout_counter = 0;
    //     }else{
    //         timeout_counter++;
    //     }
    // }

    // on_motor_move(motor_cmd_);
}

void CloberSerial::on_motor_move(MotorCommand cmd)
{

    pair<float, float> wheel_speed;
    wheel_speed = toWheelSpeed(cmd.linearVel, cmd.angularVel);
    wheel_speed.first = limitMaxSpeed(wheel_speed.first);
    wheel_speed.second = limitMaxSpeed(wheel_speed.second);

    pair<float, float> wheel_rpm;
    wheel_rpm.first = utils_.toRPM(wheel_speed.first) * 1000 / config_.MAX_RPM;
    wheel_rpm.second = utils_.toRPM(wheel_speed.second) * 1000 / config_.MAX_RPM;

    sendRPM(make_pair(0, 1), make_pair(wheel_rpm.first, wheel_rpm.second));

    if (abs(wheel_rpm.first) < 0.0001 && abs(wheel_rpm.second) < 0.0001)
    {
        sendStop(make_pair(0,1));
    }
}

void CloberSerial::sendRPM(pair<int, int> channel, pair<float, float> rpm)
{
    stringstream msg;
    msg << "!G " << channel.first + 1 << " " << rpm.first << "\r"
        << "!G " << channel.second + 1 << " " << rpm.second << "\r";
    cout << "send rpm : " << msg.str() << endl;

    serial_->write(msg.str());
}

void CloberSerial::sendStop(pair<int,int> channel){
    stringstream msg;
    msg << "!MS " << channel.first + 1 << "\r" << "!MS " << channel.second + 1 << "\r";
    serial_->write(msg.str());
}