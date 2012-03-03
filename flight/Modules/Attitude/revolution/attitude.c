/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup Attitude Copter Control Attitude Estimation
 * @brief Acquires sensor data and computes attitude estimate
 * Specifically updates the the @ref AttitudeActual "AttitudeActual" and @ref AttitudeRaw "AttitudeRaw" settings objects
 * @{
 *
 * @file       attitude.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Module to handle all comms to the AHRS on a periodic basis.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 ******************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Input objects: None, takes sensor data via pios
 * Output objects: @ref AttitudeRaw @ref AttitudeActual
 *
 * This module computes an attitude estimate from the sensor data
 *
 * The module executes in its own thread.
 *
 * UAVObjects are automatically generated by the UAVObjectGenerator from
 * the object definition XML file.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://www.openpilot.org/OpenPilot_Application_Architecture
 *
 */

#include "pios.h"
#include "accels.h"
#include "attitude.h"
#include "attitudeactual.h"
#include "attitudesettings.h"
#include "baroaltitude.h"
#include "flightstatus.h"
#include "gpsposition.h"
#include "gyros.h"
#include "gyrosbias.h"
#include "homelocation.h"
#include "magnetometer.h"
#include "nedposition.h"
#include "positionactual.h"
#include "revocalibration.h"
#include "revosettings.h"
#include "velocityactual.h"
#include "CoordinateConversions.h"

// Private constants
#define STACK_SIZE_BYTES 1540
#define TASK_PRIORITY (tskIDLE_PRIORITY+3)
#define FAILSAFE_TIMEOUT_MS 10

#define F_PI 3.14159265358979323846f
#define PI_MOD(x) (fmodf(x + F_PI, F_PI * 2) - F_PI)
// Private types

// Private variables
static xTaskHandle attitudeTaskHandle;

static xQueueHandle gyroQueue;
static xQueueHandle accelQueue;
static xQueueHandle magQueue;
static xQueueHandle baroQueue;
static xQueueHandle gpsQueue;

static AttitudeSettingsData attitudeSettings;
static HomeLocationData homeLocation;
static RevoCalibrationData revoCalibration;
static RevoSettingsData revoSettings;
const uint32_t SENSOR_QUEUE_SIZE = 10;

// Private functions
static void AttitudeTask(void *parameters);

static int32_t updateAttitudeComplimentary(bool first_run);
static int32_t updateAttitudeINSGPS(bool first_run, bool outdoor_mode);
static void settingsUpdatedCb(UAVObjEvent * objEv);

static int32_t getNED(GPSPositionData * gpsPosition, float * NED);

/**
 * API for sensor fusion algorithms:
 * Configure(xQueueHandle gyro, xQueueHandle accel, xQueueHandle mag, xQueueHandle baro)
 *   Stores all the queues the algorithm will pull data from
 * FinalizeSensors() -- before saving the sensors modifies them based on internal state (gyro bias)
 * Update() -- queries queues and updates the attitude estiamte
 */


/**
 * Initialise the module.  Called before the start function
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t AttitudeInitialize(void)
{
	AttitudeActualInitialize();
	AttitudeSettingsInitialize();
	NEDPositionInitialize();
	PositionActualInitialize();
	VelocityActualInitialize();
	RevoSettingsInitialize();
	
	// Initialize this here while we aren't setting the homelocation in GPS
	HomeLocationInitialize();

	// Initialize quaternion
	AttitudeActualData attitude;
	AttitudeActualGet(&attitude);
	attitude.q1 = 1;
	attitude.q2 = 0;
	attitude.q3 = 0;
	attitude.q4 = 0;
	AttitudeActualSet(&attitude);
	
	// Cannot trust the values to init right above if BL runs
	GyrosBiasData gyrosBias;
	GyrosBiasGet(&gyrosBias);
	gyrosBias.x = 0;
	gyrosBias.y = 0;
	gyrosBias.z = 0;
	GyrosBiasSet(&gyrosBias);
	
	AttitudeSettingsConnectCallback(&settingsUpdatedCb);
	RevoSettingsConnectCallback(&settingsUpdatedCb);
	HomeLocationConnectCallback(&settingsUpdatedCb);

	return 0;
}

/**
 * Start the task.  Expects all objects to be initialized by this point.
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t AttitudeStart(void)
{
	// Create the queues for the sensors
	gyroQueue = xQueueCreate(1, sizeof(UAVObjEvent));
	accelQueue = xQueueCreate(1, sizeof(UAVObjEvent));
	magQueue = xQueueCreate(1, sizeof(UAVObjEvent));
	baroQueue = xQueueCreate(1, sizeof(UAVObjEvent));
	gpsQueue = xQueueCreate(1, sizeof(UAVObjEvent));

	// Start main task
	xTaskCreate(AttitudeTask, (signed char *)"Attitude", STACK_SIZE_BYTES/4, NULL, TASK_PRIORITY, &attitudeTaskHandle);
	TaskMonitorAdd(TASKINFO_RUNNING_ATTITUDE, attitudeTaskHandle);
	PIOS_WDG_RegisterFlag(PIOS_WDG_ATTITUDE);
	
	GyrosConnectQueue(gyroQueue);
	AccelsConnectQueue(accelQueue);
	MagnetometerConnectQueue(magQueue);
	BaroAltitudeConnectQueue(baroQueue);
	GPSPositionConnectQueue(gpsQueue);
	
	return 0;
}

MODULE_INITCALL(AttitudeInitialize, AttitudeStart)

/**
 * Module thread, should not return.
 */
static void AttitudeTask(void *parameters)
{
	bool first_run;
	uint32_t last_algorithm;
	AlarmsClear(SYSTEMALARMS_ALARM_ATTITUDE);

	// Force settings update to make sure rotation loaded
	settingsUpdatedCb(AttitudeSettingsHandle());
	settingsUpdatedCb(RevoSettingsHandle());

	// Wait for all the sensors be to read
	vTaskDelay(100);

	// Invalidate previous algorithm to trigger a first run
	last_algorithm = 0xfffffff;

	// Main task loop
	while (1) {

		if (last_algorithm != revoSettings.FusionAlgorithm) {
			last_algorithm = revoSettings.FusionAlgorithm;
			first_run = true;
		}

		// This  function blocks on data queue
		switch (revoSettings.FusionAlgorithm ) {
			case REVOSETTINGS_FUSIONALGORITHM_COMPLIMENTARY:
				updateAttitudeComplimentary(first_run);
				break;
			case REVOSETTINGS_FUSIONALGORITHM_INSOUTDOOR:
				updateAttitudeINSGPS(first_run, true);
				break;
			case REVOSETTINGS_FUSIONALGORITHM_INSINDOOR:
				updateAttitudeINSGPS(first_run, false);
				break;
			default:
				AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE,SYSTEMALARMS_ALARM_ERROR);
				break;
		}

		first_run = false;

		PIOS_WDG_UpdateFlag(PIOS_WDG_ATTITUDE);
	}
}

float accel_mag;
float qmag;
float attitudeDt;
float mag_err[3];
float magKi = 0.000001f;
float magKp = 0.0001f;

static int32_t updateAttitudeComplimentary(bool first_run)
{
	UAVObjEvent ev;
	GyrosData gyrosData;
	AccelsData accelsData;
	static int32_t timeval;
	float dT;
	static uint8_t init = 0;

	// Wait until the AttitudeRaw object is updated, if a timeout then go to failsafe
	if ( xQueueReceive(gyroQueue, &ev, FAILSAFE_TIMEOUT_MS / portTICK_RATE_MS) != pdTRUE )
	{
		AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE,SYSTEMALARMS_ALARM_WARNING);
		return -1;
	}
	if ( xQueueReceive(accelQueue, &ev, 0) != pdTRUE )
	{
		// When one of these is updated so should the other
		AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE,SYSTEMALARMS_ALARM_WARNING);
		return -1;
	}
	
	// During initialization and 
	FlightStatusData flightStatus;
	FlightStatusGet(&flightStatus);
	if(first_run)
		init = 0;

	if((init == 0 && xTaskGetTickCount() < 7000) && (xTaskGetTickCount() > 1000)) {
		// For first 7 seconds use accels to get gyro bias
		attitudeSettings.AccelKp = 1;
		attitudeSettings.AccelKi = 0.9;
		attitudeSettings.YawBiasRate = 0.23;
	} else if ((attitudeSettings.ZeroDuringArming == ATTITUDESETTINGS_ZERODURINGARMING_TRUE) && (flightStatus.Armed == FLIGHTSTATUS_ARMED_ARMING)) {
		attitudeSettings.AccelKp = 1;
		attitudeSettings.AccelKi = 0.9;
		attitudeSettings.YawBiasRate = 0.23;
		init = 0;
	} else if (init == 0) {
		// Reload settings (all the rates)
		AttitudeSettingsGet(&attitudeSettings);
		init = 1;
	}	

	GyrosGet(&gyrosData);
	AccelsGet(&accelsData);

	// Compute the dT using the cpu clock
	dT = PIOS_DELAY_DiffuS(timeval) / 1000000.0f;
	timeval = PIOS_DELAY_GetRaw();

	float q[4];

	AttitudeActualData attitudeActual;
	AttitudeActualGet(&attitudeActual);

	float grot[3];
	float accel_err[3];

	// Get the current attitude estimate
	quat_copy(&attitudeActual.q1, q);

	// Rotate gravity to body frame and cross with accels
	grot[0] = -(2 * (q[1] * q[3] - q[0] * q[2]));
	grot[1] = -(2 * (q[2] * q[3] + q[0] * q[1]));
	grot[2] = -(q[0] * q[0] - q[1]*q[1] - q[2]*q[2] + q[3]*q[3]);
	CrossProduct((const float *) &accelsData.x, (const float *) grot, accel_err);

	// Account for accel magnitude
	accel_mag = accelsData.x*accelsData.x + accelsData.y*accelsData.y + accelsData.z*accelsData.z;
	accel_mag = sqrtf(accel_mag);
	accel_err[0] /= accel_mag;
	accel_err[1] /= accel_mag;
	accel_err[2] /= accel_mag;	

	if ( xQueueReceive(magQueue, &ev, 0) != pdTRUE )
	{
		// Rotate gravity to body frame and cross with accels
		float brot[3];
		float Rbe[3][3];
		MagnetometerData mag;
		HomeLocationData home;

		Quaternion2R(q, Rbe);
		MagnetometerGet(&mag);
		HomeLocationGet(&home);
		rot_mult(Rbe, home.Be, brot);

		float mag_len = sqrtf(mag.x * mag.x + mag.y * mag.y + mag.z * mag.z);
		mag.x /= mag_len;
		mag.y /= mag_len;
		mag.z /= mag_len;

		float bmag = sqrtf(brot[0] * brot[0] + brot[1] * brot[1] + brot[2] * brot[2]);
		brot[0] /= bmag;
		brot[1] /= bmag;
		brot[2] /= bmag;

		// Only compute if neither vector is null
		if (bmag < 1 || mag_len < 1)
			mag_err[0] = mag_err[1] = mag_err[2] = 0;
		else
			CrossProduct((const float *) &mag.x, (const float *) brot, mag_err);
	} else {
		mag_err[0] = mag_err[1] = mag_err[2] = 0;
	}

	// Accumulate integral of error.  Scale here so that units are (deg/s) but Ki has units of s
	GyrosBiasData gyrosBias;
	GyrosBiasGet(&gyrosBias);
	gyrosBias.x += accel_err[0] * attitudeSettings.AccelKi;
	gyrosBias.y += accel_err[1] * attitudeSettings.AccelKi;
	gyrosBias.z += mag_err[2] * magKi;
	GyrosBiasSet(&gyrosBias);

	// Correct rates based on error, integral component dealt with in updateSensors
	gyrosData.x += accel_err[0] * attitudeSettings.AccelKp / dT;
	gyrosData.y += accel_err[1] * attitudeSettings.AccelKp / dT;
	gyrosData.z += accel_err[2] * attitudeSettings.AccelKp / dT + mag_err[2] * magKp / dT;

	// Work out time derivative from INSAlgo writeup
	// Also accounts for the fact that gyros are in deg/s
	float qdot[4];
	qdot[0] = (-q[1] * gyrosData.x - q[2] * gyrosData.y - q[3] * gyrosData.z) * dT * F_PI / 180 / 2;
	qdot[1] = (q[0] * gyrosData.x - q[3] * gyrosData.y + q[2] * gyrosData.z) * dT * F_PI / 180 / 2;
	qdot[2] = (q[3] * gyrosData.x + q[0] * gyrosData.y - q[1] * gyrosData.z) * dT * F_PI / 180 / 2;
	qdot[3] = (-q[2] * gyrosData.x + q[1] * gyrosData.y + q[0] * gyrosData.z) * dT * F_PI / 180 / 2;

	// Take a time step
	q[0] = q[0] + qdot[0];
	q[1] = q[1] + qdot[1];
	q[2] = q[2] + qdot[2];
	q[3] = q[3] + qdot[3];

	if(q[0] < 0) {
		q[0] = -q[0];
		q[1] = -q[1];
		q[2] = -q[2];
		q[3] = -q[3];
	}

	// Renomalize
	qmag = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
	q[0] = q[0] / qmag;
	q[1] = q[1] / qmag;
	q[2] = q[2] / qmag;
	q[3] = q[3] / qmag;

	// If quaternion has become inappropriately short or is nan reinit.
	// THIS SHOULD NEVER ACTUALLY HAPPEN
	if((fabs(qmag) < 1.0e-3f) || (qmag != qmag)) {
		q[0] = 1;
		q[1] = 0;
		q[2] = 0;
		q[3] = 0;
	}

	quat_copy(q, &attitudeActual.q1);

	// Convert into eueler degrees (makes assumptions about RPY order)
	Quaternion2RPY(&attitudeActual.q1,&attitudeActual.Roll);

	AttitudeActualSet(&attitudeActual);

	// Flush these queues for avoid errors
	xQueueReceive(baroQueue, &ev, 0);
	if ( xQueueReceive(gpsQueue, &ev, 0) == pdTRUE && homeLocation.Set == HOMELOCATION_SET_TRUE ) {
		float NED[3];
		// Transform the GPS position into NED coordinates
		GPSPositionData gpsPosition;
		GPSPositionGet(&gpsPosition);
		getNED(&gpsPosition, NED);

		PositionActualData positionActual;
		PositionActualGet(&positionActual);
		positionActual.North = NED[0];
		positionActual.East = NED[1];
		positionActual.Down = NED[2];
		PositionActualSet(&positionActual);

		VelocityActualData velocityActual;
		VelocityActualGet(&velocityActual);
		velocityActual.North = gpsPosition.Groundspeed * cosf(gpsPosition.Heading * F_PI / 180.0f);
		velocityActual.East = gpsPosition.Groundspeed * sinf(gpsPosition.Heading * F_PI / 180.0f);
		velocityActual.Down = 0;
		VelocityActualSet(&velocityActual);
	}

	AlarmsClear(SYSTEMALARMS_ALARM_ATTITUDE);

	return 0;
}

#include "insgps.h"
int32_t ins_failed = 0;
extern struct NavStruct Nav;
int32_t init_stage = 0;

/**
 * @brief Use the INSGPS fusion algorithm in either indoor or outdoor mode (use GPS)
 * @params[in] first_run This is the first run so trigger reinitialization
 * @params[in] outdoor_mode If true use the GPS for position, if false weakly pull to (0,0)
 * @return 0 for success, -1 for failure
 */
static int32_t updateAttitudeINSGPS(bool first_run, bool outdoor_mode)
{
	UAVObjEvent ev;
	GyrosData gyrosData;
	AccelsData accelsData;
	MagnetometerData magData;
	BaroAltitudeData baroData;
	GPSPositionData gpsData;
	GyrosBiasData gyrosBias;
	HomeLocationData home;

	static bool mag_updated;
	static bool baro_updated;
	static bool gps_updated;

	static uint32_t ins_last_time = 0;
	static bool inited;

	float NED[3] = {0.0f, 0.0f, 0.0f};
	float vel[3] = {0.0f, 0.0f, 0.0f};
	float zeros[3] = {0.0f, 0.0f, 0.0f};

	// Perform the update
	uint16_t sensors = 0;
	float dT;

	inited = first_run ? false : inited;

	// Wait until the gyro and accel object is updated, if a timeout then go to failsafe
	if ( (xQueueReceive(gyroQueue, &ev, 10 / portTICK_RATE_MS) != pdTRUE) ||
		(xQueueReceive(accelQueue, &ev, 10 / portTICK_RATE_MS) != pdTRUE) )
	{
		AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE,SYSTEMALARMS_ALARM_WARNING);
		return -1;
	}

	if (inited) {
		mag_updated = 0;
		baro_updated = 0;
		gps_updated = 0;
	}

	mag_updated |= xQueueReceive(magQueue, &ev, 0 / portTICK_RATE_MS) == pdTRUE;
	baro_updated |= xQueueReceive(baroQueue, &ev, 0 / portTICK_RATE_MS) == pdTRUE;
	gps_updated |= (xQueueReceive(gpsQueue, &ev, 0 / portTICK_RATE_MS) == pdTRUE) && outdoor_mode;

	// Get most recent data
	GyrosGet(&gyrosData);
	AccelsGet(&accelsData);
	MagnetometerGet(&magData);
	BaroAltitudeGet(&baroData);
	GPSPositionGet(&gpsData);
	HomeLocationGet(&home);

	// Have a minimum requirement for gps usage
	gps_updated &= (gpsData.Satellites >= 7) && (gpsData.PDOP <= 4.0f) && (homeLocation.Set == HOMELOCATION_SET_TRUE);

	if (!inited)
		AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE,SYSTEMALARMS_ALARM_ERROR);
	else if (outdoor_mode && gpsData.Satellites < 7)
		AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE,SYSTEMALARMS_ALARM_ERROR);
	else
		AlarmsClear(SYSTEMALARMS_ALARM_ATTITUDE);
			
	if (!inited && mag_updated && baro_updated && (gps_updated || !outdoor_mode)) {
		// Don't initialize until all sensors are read
		if (init_stage == 0 && !outdoor_mode) {
			float Pdiag[16]={25.0f,25.0f,25.0f,5.0f,5.0f,5.0f,1e-5f,1e-5f,1e-5f,1e-5f,1e-5f,1e-5f,1e-5f,1e-4f,1e-4f,1e-4f};
			float q[4];
			float pos[3] = {0.0f, 0.0f, 0.0f};
			pos[2] = baroData.Altitude * -1.0f;

			// Reset the INS algorithm
			INSGPSInit();
			INSSetMagVar(revoCalibration.mag_var);
			INSSetAccelVar(revoCalibration.accel_var);
			INSSetGyroVar(revoCalibration.gyro_var);

			// Set initial attitude
			float rpy[3];
			rpy[0] = atan2f(accelsData.x, accelsData.z) * 180.0f / F_PI;
			rpy[1] = atan2f(accelsData.y, accelsData.z) * 180.0f / F_PI;
			rpy[2] = atan2f(magData.x, -magData.y) * 180.0f / F_PI;
			RPY2Quaternion(rpy,q);
			/*float Rbe[3][3];
			float ge[3] = {0,0,-9.81f};
			RotFrom2Vectors(&accelsData.x, ge, &magData.x, home.Be, Rbe);
			R2Quaternion(Rbe,q);*/

			INSSetState(pos, zeros, q, zeros, zeros);
			INSResetP(Pdiag);
		} else if (init_stage == 0 && outdoor_mode) {
			float Pdiag[16]={25.0f,25.0f,25.0f,5.0f,5.0f,5.0f,1e-5f,1e-5f,1e-5f,1e-5f,1e-5f,1e-5f,1e-5f,1e-4f,1e-4f,1e-4f};
			float rpy[3];
			float q[4];
			float NED[3];

			// Reset the INS algorithm
			INSGPSInit();
			INSSetMagVar(revoCalibration.mag_var);
			INSSetAccelVar(revoCalibration.accel_var);
			INSSetGyroVar(revoCalibration.gyro_var);

			INSSetMagNorth(home.Be);

			GPSPositionData gpsPosition;
			GPSPositionGet(&gpsPosition);

			// Transform the GPS position into NED coordinates
			getNED(&gpsPosition, NED);

			// Set initial attitude
			rpy[0] = atan2f(accelsData.x, accelsData.z) * 180.0f / F_PI;
			rpy[1] = atan2f(accelsData.y, accelsData.z) * 180.0f / F_PI;
			rpy[2] = atan2f(magData.x, -magData.y) * 180.0f / F_PI;
			RPY2Quaternion(rpy,q);

			INSSetState(NED, zeros, q, zeros, zeros);
			INSResetP(Pdiag);
		} else if (init_stage > 0) {
			// Run prediction a bit before any corrections
			GyrosBiasGet(&gyrosBias);
			float gyros[3] = {(gyrosData.x + gyrosBias.x) * F_PI / 180.0f, 
				(gyrosData.y + gyrosBias.y) * F_PI / 180.0f, 
				(gyrosData.z + gyrosBias.z) * F_PI / 180.0f};
			INSStatePrediction(gyros, &accelsData.x, 0.002f);
		}

		init_stage++;
		if(init_stage > 10)
			inited = true;

		ins_last_time = PIOS_DELAY_GetRaw();	

		return -1;
	}

	if (!inited)
		return -1;

	dT = PIOS_DELAY_DiffuS(ins_last_time) / 1.0e6f;
	ins_last_time = PIOS_DELAY_GetRaw();

	// This should only happen at start up or at mode switches
	if(dT > 0.01f)
		dT = 0.01f;
	else if(dT <= 0.001f)
		dT = 0.001f;

	// Because the sensor module remove the bias we need to add it
	// back in here so that the INS algorithm can track it correctly 
	GyrosBiasGet(&gyrosBias);
	float gyros[3] = {(gyrosData.x + gyrosBias.x) * F_PI / 180.0f, 
		(gyrosData.y + gyrosBias.y) * F_PI / 180.0f, 
		(gyrosData.z + gyrosBias.z) * F_PI / 180.0f};

	// Advance the state estimate
	INSStatePrediction(gyros, &accelsData.x, dT);

	// Copy the attitude into the UAVO
	AttitudeActualData attitude;
	AttitudeActualGet(&attitude);
	attitude.q1 = Nav.q[0];
	attitude.q2 = Nav.q[1];
	attitude.q3 = Nav.q[2];
	attitude.q4 = Nav.q[3];
	Quaternion2RPY(&attitude.q1,&attitude.Roll);
	AttitudeActualSet(&attitude);

	// Copy the gyro bias into the UAVO
	gyrosBias.x = Nav.gyro_bias[0];
	gyrosBias.y = Nav.gyro_bias[1];
	gyrosBias.z = Nav.gyro_bias[2];
	GyrosBiasSet(&gyrosBias);

	// Advance the covariance estimate
	INSCovariancePrediction(dT);

	if(mag_updated)
		sensors |= MAG_SENSORS;

	if(baro_updated)
		sensors |= BARO_SENSOR;

	INSSetMagNorth(home.Be);

	if(gps_updated && outdoor_mode)
	{
		INSSetPosVelVar(1e-2f, 1e-2f);
		sensors = POS_SENSORS | HORIZ_SENSORS;
		GPSPositionData gpsPosition;
		GPSPositionGet(&gpsPosition);

		vel[0] = gpsPosition.Groundspeed * cosf(gpsPosition.Heading * F_PI / 180.0f);
		vel[1] = gpsPosition.Groundspeed * sinf(gpsPosition.Heading * F_PI / 180.0f);
		vel[2] = 0;

		// Transform the GPS position into NED coordinates
		getNED(&gpsPosition, NED);

		// Store this for inspecting offline
		NEDPositionData nedPos;
		NEDPositionGet(&nedPos);
		nedPos.North = NED[0];
		nedPos.East = NED[1];
		nedPos.Down = NED[2];
		NEDPositionSet(&nedPos);

	} else if (!outdoor_mode) {
		INSSetPosVelVar(1e-2f, 1e-2f);
		vel[0] = vel[1] = vel[2] = 0;
		NED[0] = NED[1] = 0;
		NED[2] = baroData.Altitude;
		sensors |= HORIZ_SENSORS | HORIZ_POS_SENSORS;
	}

	/*
	 * TODO: Need to add a general sanity check for all the inputs to make sure their kosher
	 * although probably should occur within INS itself
	 */
	if (sensors)
		INSCorrection(&magData.x, NED, vel, baroData.Altitude, sensors);

	// Copy the position and velocity into the UAVO
	PositionActualData positionActual;
	PositionActualGet(&positionActual);
	positionActual.North = Nav.Pos[0];
	positionActual.East = Nav.Pos[1];
	positionActual.Down = Nav.Pos[2];
	PositionActualSet(&positionActual);
	
	VelocityActualData velocityActual;
	VelocityActualGet(&velocityActual);
	velocityActual.North = Nav.Vel[0];
	velocityActual.East = Nav.Vel[1];
	velocityActual.Down = Nav.Vel[2];
	VelocityActualSet(&velocityActual);
	
	if(fabs(Nav.gyro_bias[0]) > 0.1f || fabs(Nav.gyro_bias[1]) > 0.1f || fabs(Nav.gyro_bias[2]) > 0.1f) {
		float zeros[3] = {0.0f,0.0f,0.0f};
		INSSetGyroBias(zeros);
	}

	return 0;
}

/**
 * @brief Convert the GPS LLA position into NED coordinates
 * @note this method uses a taylor expansion around the home coordinates
 * to convert to NED which allows it to be done with all floating
 * calculations
 * @param[in] Current GPS coordinates
 * @param[out] NED frame coordinates
 * @returns 0 for success, -1 for failure
 */
float T[3];
const float DEG2RAD = 3.141592653589793f / 180.0f;
static int32_t getNED(GPSPositionData * gpsPosition, float * NED)
{
	float dL[3] = {(gpsPosition->Latitude - homeLocation.Latitude) / 10.0e6f * DEG2RAD,
		(gpsPosition->Longitude - homeLocation.Longitude) / 10.0e6f * DEG2RAD,
		(gpsPosition->Altitude + gpsPosition->GeoidSeparation - homeLocation.Altitude)};

	NED[0] = T[0] * dL[0];
	NED[1] = T[1] * dL[1];
	NED[2] = T[2] * dL[2];

	return 0;
}

static void settingsUpdatedCb(UAVObjEvent * objEv) 
{
	float lat, alt;

	AttitudeSettingsGet(&attitudeSettings);
	RevoCalibrationGet(&revoCalibration);
	RevoSettingsGet(&revoSettings);
	HomeLocationGet(&homeLocation);

	GyrosBiasData gyrosBias;
	GyrosBiasGet(&gyrosBias);
	gyrosBias.x = attitudeSettings.GyroBias[ATTITUDESETTINGS_GYROBIAS_X] / 100.0f;
	gyrosBias.y = attitudeSettings.GyroBias[ATTITUDESETTINGS_GYROBIAS_Y] / 100.0f;
	gyrosBias.z = attitudeSettings.GyroBias[ATTITUDESETTINGS_GYROBIAS_Z] / 100.0f;
	GyrosBiasSet(&gyrosBias);

	// Compute matrix to convert deltaLLA to NED
	lat = homeLocation.Latitude / 10.0e6f * DEG2RAD;
	alt = homeLocation.Altitude;

	// In case INS currently running
	INSSetMagVar(revoCalibration.mag_var);
	INSSetAccelVar(revoCalibration.accel_var);
	INSSetGyroVar(revoCalibration.gyro_var);

	T[0] = alt+6.378137E6f;
	T[1] = cosf(lat)*(alt+6.378137E6f);
	T[2] = -1.0f;
}
/**
 * @}
 * @}
 */
