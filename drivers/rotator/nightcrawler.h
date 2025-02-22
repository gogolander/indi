/*
    NightCrawler NightCrawler Focuser & Rotator
    Copyright (C) 2017 Jasem Mutlaq (mutlaqja@ikarustech.com)

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

*/

#pragma once

#include "indifocuser.h"
#include "indirotatorinterface.h"

class NightCrawler : public INDI::Focuser, public INDI::RotatorInterface
{
    public:

        typedef enum { MOTOR_FOCUS, MOTOR_ROTATOR, MOTOR_AUX } MotorType;

        NightCrawler();
        virtual ~NightCrawler() = default;

        virtual bool Handshake() override;
        const char * getDefaultName() override;
        virtual bool initProperties() override;
        virtual bool updateProperties() override;
        virtual bool ISNewNumber (const char * dev, const char * name, double values[], char * names[], int n) override;
        virtual bool ISNewSwitch (const char * dev, const char * name, ISState * states, char * names[], int n) override;

        static void abnormalDisconnectCallback(void *userpointer);

    protected:
        // Focuser
        virtual IPState MoveAbsFocuser(uint32_t targetTicks) override;
        virtual IPState MoveRelFocuser(FocusDirection dir, uint32_t ticks) override;
        virtual bool AbortFocuser() override;


        // Rotator
        virtual IPState HomeRotator() override;
        virtual IPState MoveRotator(double angle) override;
        virtual bool SyncRotator(double angle) override;
        virtual bool AbortRotator() override;

        // Misc.
        virtual bool saveConfigItems(FILE *fp) override;
        virtual void TimerHit() override;

    private:
        // Get Firmware
        bool getFirmware();
        // Get Focuer Type
        bool getFocuserType();
        // Check if connection is OK
        bool Ack();
        // Goto Position
        bool gotoMotor(MotorType type, int32_t position);
        // Get Position
        bool getPosition(MotorType type);
        // Sync to Position
        bool syncMotor(MotorType type, uint32_t position);
        // Start/Stop Motors
        bool startMotor(MotorType type);
        bool stopMotor(MotorType type);
        bool isMotorMoving(MotorType type);
        // Sensors (Temperature + Voltage)
        bool getTemperature();
        bool getVoltage();
        // Abnormal disconnect
        void abnormalDisconnect();
        // Temperature offset
        bool setTemperatureOffset(double offset);
        // Motor step rate in 100 microsecond intervals
        bool getStepDelay(MotorType type);
        bool setStepDelay(MotorType type, uint32_t delay);
        // Limit Switch
        bool getLimitSwitchStatus();
        // Home
        bool findHome(uint8_t motorTypes);
        bool isHomingComplete();
        // Encoders
        bool setEncodersEnabled(bool enable);
        // Brightness
        bool setDisplayBrightness(uint8_t value);
        bool setSleepBrightness(uint8_t value);


        INumber GotoAuxN[1];
        INumberVectorProperty GotoAuxNP;

        INumber SyncFocusN[1];
        INumberVectorProperty SyncFocusNP;

        INumber SyncAuxN[1];
        INumberVectorProperty SyncAuxNP;

        ISwitch AbortAuxS[1];
        ISwitchVectorProperty AbortAuxSP;

        INumber VoltageN[1];
        INumberVectorProperty VoltageNP;

        INumber TemperatureN[1];
        INumberVectorProperty TemperatureNP;

        INumber TemperatureOffsetN[1];
        INumberVectorProperty TemperatureOffsetNP;

        INumber FocusStepDelayN[1];
        INumberVectorProperty FocusStepDelayNP;
        INumber RotatorStepDelayN[1];
        INumberVectorProperty RotatorStepDelayNP;
        INumber AuxStepDelayN[1];
        INumberVectorProperty AuxStepDelayNP;

        ILight LimitSwitchL[3];
        ILightVectorProperty LimitSwitchLP;
        enum { ROTATION_SWITCH, OUT_SWITCH, IN_SWITCH };

        ISwitch HomeSelectionS[3];
        ISwitchVectorProperty HomeSelectionSP;
        ISwitch FindHomeS[1];
        ISwitchVectorProperty FindHomeSP;

        ISwitch EncoderS[2];
        ISwitchVectorProperty EncoderSP;

        INumber BrightnessN[2];
        INumberVectorProperty BrightnessNP;
        enum { BRIGHTNESS_DISPLAY, BRIGHTNESS_SLEEP };

        // Rotator Steps
        INumber RotatorAbsPosN[1];
        INumberVectorProperty RotatorAbsPosNP;

        double lastTemperature { 0 };
        double lastVoltage { 0 };
        uint32_t m_RotatorTicksPerDegree { 0 };
        int32_t m_RotatorStepsPerRevolution {0};
        double lastFocuserPosition { 0 };
        double lastRotatorPosition { 0 };
        double lastAuxPosition { 0 };
        double targetPosition { 0 };
        IPState rotationLimit { IPS_IDLE };
        IPState outSwitchLimit { IPS_IDLE };
        IPState inSwitchLimit { IPS_IDLE };
};
