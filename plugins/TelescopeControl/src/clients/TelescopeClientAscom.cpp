/*
 * Stellarium Telescope Control plug-in
 * Copyright (C) 2010  Bogdan Marinov (this file)
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "TelescopeClientAscom.hpp"

#include <cmath>

#include <QAxObject>

#include "StelUtils.hpp"

const char* TelescopeClientAscom::P_CONNECTED = "Connected";
const char* TelescopeClientAscom::P_PARKED = "AtPark";
const char* TelescopeClientAscom::P_TRACKING = "Tracking";
const char* TelescopeClientAscom::P_CAN_SLEW = "CanSlew";
const char* TelescopeClientAscom::P_CAN_SLEW_ASYNCHRONOUSLY = "CanSlewAsync";
const char* TelescopeClientAscom::P_CAN_TRACK = "CanSetTracking";
const char* TelescopeClientAscom::P_CAN_UNPARK = "CanUnpark";
const char* TelescopeClientAscom::P_RA = "RightAscension";
const char* TelescopeClientAscom::P_DEC = "Declination";
const char* TelescopeClientAscom::P_EQUATORIAL_SYSTEM = "EquatorialSystem";
const char* TelescopeClientAscom::M_UNPARK = "Unpark()";
const char* TelescopeClientAscom::M_SLEW = "SlewToCoordinates(double, double)";
const char* TelescopeClientAscom::M_SLEW_ASYNCHRONOUSLY =
		"SlewToCoordinatesAsync(double, double)";

TelescopeClientAscom::TelescopeClientAscom(const QString &name, const QString &params, Equinox eq):
		TelescopeClient(name),
		equinox(eq),
		driver(0)
{
	qDebug() << "Creating ASCOM telescope client:" << name << params;

	//Get the driver identifier from the parameters string
	//(for now, it contains only the driver identifier)
	driverId = params;

	//Intialize the driver object
	driver = new QAxObject(this);
	driver->setControl(driverId);
	if (driver->isNull())
		return;
	connect(driver,
			SIGNAL(exception (int, QString, QString, QString)),
			this,
			SLOT(handleDriverException(int, QString, QString, QString)));

	//Check if the driver supports slewing to an equatorial position
	bool canSlew = driver->property(P_CAN_SLEW).toBool();
	if (!canSlew)
		qWarning() << "Warning!" << name << "can't receive \"go to\" commands. "
		              "Its current position will be displayed only.";
	//(Not an error - this covers things like digital setting circles that can
	// only emit their current position)

	//Try to connect (make sure driver settings are correct, e.g. the serial
	//port is the right one)
	driver->setProperty(P_CONNECTED, true);
	bool connectionAttemptSucceeded = driver->property(P_CONNECTED).toBool();
	if (!connectionAttemptSucceeded)
	{
		deleteDriver();
	}

	//If it is parked, see if it can be unparked
	//TODO: Temporary. The imporved GUI should offer parking/unparking.
	bool isParked = driver->property(P_PARKED).toBool();
	if (isParked)
	{
		bool canUnpark = driver->property(P_CAN_UNPARK).toBool();
		if (!canUnpark)
		{
			qDebug() << "The" << name << "telescope is parked"
			         << "and the Telescope control plug-in can't unpark it.";
			deleteDriver();
		}
	}

	//Initialize the countdown
	timeToGetPosition = getNow() + POSITION_REFRESH_INTERVAL;
}

TelescopeClientAscom::~TelescopeClientAscom(void)
{
	if (driver)
	{
		if (!driver->isNull())
			driver->clear();
		delete driver;
		driver = 0;
	}
}

bool TelescopeClientAscom::isInitialized(void) const
{
	if (driver && !driver->isNull())
		return true;
	else
		return false;
}

bool TelescopeClientAscom::isConnected(void) const
{
	if (!isInitialized())
		return false;

	bool isConnected = driver->property(P_CONNECTED).toBool();
	return isConnected;
}

Vec3d TelescopeClientAscom::getJ2000EquatorialPos(const StelNavigator *nav) const
{
	//TODO: see what to do about time_delay
	const qint64 now = getNow() - POSITION_REFRESH_INTERVAL;// - time_delay;
	return interpolatedPosition.get(now);
}

bool TelescopeClientAscom::prepareCommunication()
{
	if (!isInitialized())
		return false;

	return true;
}

void TelescopeClientAscom::performCommunication()
{
	if (!isInitialized())
		return;

	if (!isConnected())
	{
		driver->setProperty(P_CONNECTED, true);
		bool connectionAttemptSucceeded = driver->property(P_CONNECTED).toBool();
		if (!connectionAttemptSucceeded)
			return;
	}

	bool isParked = driver->property(P_PARKED).toBool();
	if (isParked)
		return;

	//Get the position every POSITION_REFRESH_INTERVAL microseconds
	const qint64 now = getNow();
	if (now < timeToGetPosition)
		return;
	else
		timeToGetPosition = now + POSITION_REFRESH_INTERVAL;

	//This returns result of type AscomInterfacesLib::EquatorialCoordinateType,
	//which is not registered. AFAIK, using it would require defining
	//the interface in Stellarium's code. I don't know how compatible is this
	//with the GNU GPL.
	/*
	//Determine the coordinate system
	//Stellarium supports only JNow (1) and J2000 (2).
	int equatorialCoordinateType = driver->property(P_EQUATORIAL_SYSTEM).toInt();
	if (equatorialCoordinateType < 1 || equatorialCoordinateType > 2)
	{
		qWarning() << "Stellarium does not support any of the coordinate "
		              "formats used by" << name;
		return;
	}*/

	//Get the coordinates and convert them to a vector
	const qint64 serverTime = getNow();
	const double raHours = driver->property(P_RA).toDouble();
	const double decDegrees = driver->property(P_DEC).toDouble();
	const double raRadians = raHours * (M_PI / 12);
	const double decRadians = decDegrees * (M_PI / 180);
	Vec3d coordinates;
	StelUtils::spheToRect(raRadians, decRadians, coordinates);

	Vec3d j2000Coordinates = coordinates;
	//See the note about equinox detection above:
	//if (equatorialCoordinateType == 1)//coordinates are in JNow
	if (equinox == EquinoxJNow)
	{
		const StelNavigator* navigator = StelApp::getInstance().getCore()->getNavigator();
		j2000Coordinates = navigator->equinoxEquToJ2000(coordinates);
	}

	interpolatedPosition.add(j2000Coordinates, getNow(), serverTime);
}

void TelescopeClientAscom::telescopeGoto(const Vec3d &j2000Coordinates)
{
	if (!isInitialized())
		return;

	if (!isConnected())
	{
		driver->setProperty(P_CONNECTED, true);
		bool connectionAttemptSucceeded = driver->property(P_CONNECTED).toBool();
		if (!connectionAttemptSucceeded)
			return;
	}

	//This returns result of type AscomInterfacesLib::EquatorialCoordinateType,
	//which is not registered. AFAIK, using it would require defining
	//the interface in Stellarium's code. I don't know how compatible is this
	//with the GNU GPL.
	/*
	//Determine the coordinate system
	//Stellarium supports only JNow (1) and J2000 (2).
	int equatorialCoordinateType = driver->property(P_EQUATORIAL_SYSTEM).toInt();
	if (equatorialCoordinateType < 1 || equatorialCoordinateType > 2)
	{
		qWarning() << "Stellarium does not support any of the coordinate "
		              "formats used by" << name;
		return;
	}
	*/

	//Parked?
	bool isParked = driver->property(P_PARKED).toBool();
	if (isParked)
	{
		bool canUnpark = driver->property(P_CAN_UNPARK).toBool();
		if (canUnpark)
		{
			driver->dynamicCall(M_UNPARK);
		}
		else
		{
			qDebug() << "The" << name << "telescope is parked"
			         << "and the Telescope control plug-in can't unpark it.";
			return;
		}
	}

	//Tracking?
	bool isTracking = driver->property(P_TRACKING).toBool();
	if (!isTracking)
	{
		bool canTrack = driver->property(P_CAN_TRACK).toBool();
		if (canTrack)
		{
			driver->setProperty(P_TRACKING, true);
			isTracking = driver->property(P_TRACKING).toBool();
			if (!isTracking)
				return;
		}
		else
		{
			//TODO: Are there any drivers that can slew, but not track?
			return;
		}
	}

	//Equatorial system
	Vec3d targetCoordinates = j2000Coordinates;
	//See the note about equinox detection above:
	//if (equatorialCoordinateType == 1)//coordinates are in JNow
	if (equinox == EquinoxJNow)
	{
		const StelNavigator* navigator = StelApp::getInstance().getCore()->getNavigator();
		targetCoordinates = navigator->equinoxEquToJ2000(j2000Coordinates);
	}

	//Convert coordinates from the vector
	double raRadians;
	double decRadians;
	StelUtils::rectToSphe(&raRadians, &decRadians, targetCoordinates);
	const double raHours = raRadians * (12 / M_PI);
	const double decDegrees = decRadians * (180 / M_PI);

	//Send the "go to" command
	bool canSlewAsynchronously = driver->property(P_CAN_SLEW_ASYNCHRONOUSLY).toBool();
	if (canSlewAsynchronously)
	{
		driver->dynamicCall(M_SLEW_ASYNCHRONOUSLY, raHours, decDegrees);
	}
	else
	{
		//Last resort - this block the execution of Stellarium until the
		//slew is complete.
		bool canSlew = driver->property(P_CAN_SLEW).toBool();
		if (canSlew)
		{
			driver->dynamicCall(M_SLEW, raHours, decDegrees);
		}
	}
}

void TelescopeClientAscom::handleDriverException(int code,
												 const QString &source,
												 const QString &desc,
												 const QString &help)
{
	QString errorMessage = QString("%1: ASCOM driver error:\n"
	                               "Code: %2\n"
	                               "Source: %3\n"
	                               "Description: %4")
	                               .arg(name)
	                               .arg(code)
	                               .arg(desc);
	qDebug() << errorMessage;
	deleteDriver();
	emit ascomError(errorMessage);
}

void TelescopeClientAscom::deleteDriver()
{
	if (driver)
	{
		delete driver;
		driver = 0;
	}
	return;
}
