/*
 * Stellarium
 * Copyright (C) 2009, 2012 Matthew Gates
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA  02110-1335, USA.
 */

//#define IRIDIUM_SAT_TEXT_DEBUG


#include "Satellite.hpp"
#include "StelObject.hpp"
#include "StelPainter.hpp"
#include "StelApp.hpp"
#include "StelLocation.hpp"
#include "StelCore.hpp"
#include "StelTexture.hpp"
#include "VecMath.hpp"
#include "StelUtils.hpp"
#include "StelTranslator.hpp"

#include <QTextStream>
#include <QRegExp>
#include <QDebug>
#include <QVariant>
#include <QSettings>
#include <QByteArray>

#include <QVector3D>
#include <QMatrix4x4>

#include "gsatellite/gTime.hpp"
#include "gsatellite/stdsat.h"

#include <cmath>

#define sqr(a) ((a)*(a))

// static data members - will be initialised in the Satallites class (the StelObjectMgr)
StelTextureSP Satellite::hintTexture;
float Satellite::showLabels = true;
float Satellite::hintBrightness = 0.0;
float Satellite::hintScale = 1.f;
SphericalCap Satellite::viewportHalfspace = SphericalCap();
int Satellite::orbitLineSegments = 90;
int Satellite::orbitLineFadeSegments = 4;
int Satellite::orbitLineSegmentDuration = 20;
bool Satellite::orbitLinesFlag = true;
bool Satellite::realisticModeFlag = false;

#ifdef IRIDIUM_SAT_TEXT_DEBUG
QString Satellite::myText = "";
#endif
double Satellite::sunReflAngle = 180.;


Satellite::Satellite(const QString& identifier, const QVariantMap& map)
    : initialized(false),
      displayed(true),
      orbitDisplayed(false),
      userDefined(false),
      newlyAdded(false),
      orbitValid(false),
      jdLaunchYearJan1(0),
      stdMag(99.),
      height(0.),
      range(0.),
      rangeRate(0.),
      hintColor(0.0,0.0,0.0),            
      lastUpdated(),
      pSatWrapper(NULL),
      visibility(0),
      phaseAngle(0.),
      lastEpochCompForOrbit(0.),
      epochTime(0.)

{
	// return initialized if the mandatory fields are not present
	if (identifier.isEmpty())
		return;
	if (!map.contains("name") || !map.contains("tle1") || !map.contains("tle2"))
		return;

	// Font size is 16
	font.setPixelSize(StelApp::getInstance().getBaseFontSize()+3);

	id = identifier;
	name  = map.value("name").toString();
	if (name.isEmpty())
		return;
	
	// If there are no such keys, these will be initialized with the default
	// values given them above.
	description = map.value("description", description).toString().trimmed();
	displayed = map.value("visible", displayed).toBool();
	orbitDisplayed = map.value("orbitVisible", orbitDisplayed).toBool();
	userDefined = map.value("userDefined", userDefined).toBool();
	stdMag = map.value("stdMag", 99.f).toDouble();
	// Satellite hint color
	QVariantList list = map.value("hintColor", QVariantList()).toList();
	if (list.count() == 3)
	{
		hintColor[0] = list.at(0).toDouble();
		hintColor[1] = list.at(1).toDouble();
		hintColor[2] = list.at(2).toDouble();
	}
	
	// Satellite orbit section color
	list = map.value("orbitColor", QVariantList()).toList();
	if (list.count() == 3)
	{
		orbitColor[0] = list.at(0).toDouble();
		orbitColor[1] = list.at(1).toDouble();
		orbitColor[2] = list.at(2).toDouble();
	}
	else
	{
		orbitColor = hintColor;
	}

	if (map.contains("comms"))
	{
		foreach(const QVariant &comm, map.value("comms").toList())
		{
			QVariantMap commMap = comm.toMap();
			CommLink c;
			if (commMap.contains("frequency")) c.frequency = commMap.value("frequency").toDouble();
			if (commMap.contains("modulation")) c.modulation = commMap.value("modulation").toString();
			if (commMap.contains("description")) c.description = commMap.value("description").toString();
			comms.append(c);
		}
	}

	QVariantList groupList =  map.value("groups", QVariantList()).toList();
	if (!groupList.isEmpty())
	{
		foreach(const QVariant& group, groupList)
			groups.insert(group.toString());
	}

	// TODO: Somewhere here - some kind of TLE validation.
	QString line1 = map.value("tle1").toString();
	QString line2 = map.value("tle2").toString();
	setNewTleElements(line1, line2);
	// This also sets the international designator and launch year.

	QString dateString = map.value("lastUpdated").toString();
	if (!dateString.isEmpty())
		lastUpdated = QDateTime::fromString(dateString, Qt::ISODate);

	orbitValid = true;
	initialized = true;

	update(0.);
}

Satellite::~Satellite()
{
	if (pSatWrapper != NULL)
	{

		delete pSatWrapper;
		pSatWrapper = NULL;
	}
}

double Satellite::roundToDp(float n, int dp)
{
	// round n to dp decimal places
	return floor(n * pow(10., dp) + .5) / pow(10., dp);
}

QVariantMap Satellite::getMap(void)
{
	QVariantMap map;
	map["name"] = name;	
	map["stdMag"] = stdMag;
	map["tle1"] = tleElements.first.data();
	map["tle2"] = tleElements.second.data();

	if (!description.isEmpty())
		map["description"] = description;

	map["visible"] = displayed;
	map["orbitVisible"] = orbitDisplayed;
	if (userDefined)
		map.insert("userDefined", userDefined);
	QVariantList col, orbitCol;
	col << roundToDp(hintColor[0],3) << roundToDp(hintColor[1], 3) << roundToDp(hintColor[2], 3);
	orbitCol << roundToDp(orbitColor[0], 3) << roundToDp(orbitColor[1], 3) << roundToDp(orbitColor[2],3);
	map["hintColor"] = col;
	map["orbitColor"] = orbitCol;
	QVariantList commList;
	foreach(const CommLink &c, comms)
	{
		QVariantMap commMap;
		commMap["frequency"] = c.frequency;
		if (!c.modulation.isEmpty()) commMap["modulation"] = c.modulation;
		if (!c.description.isEmpty()) commMap["description"] = c.description;
		commList << commMap;
	}
	map["comms"] = commList;
	QVariantList groupList;
	foreach(const QString &g, groups)
	{
		groupList << g;
	}
	map["groups"] = groupList;

	if (!lastUpdated.isNull())
	{
		// A raw QDateTime is not a recognised JSON data type. --BM
		map["lastUpdated"] = lastUpdated.toString(Qt::ISODate);
	}

	return map;
}

float Satellite::getSelectPriority(const StelCore*) const
{
	return -10.;
}

QString Satellite::getInfoString(const StelCore *core, const InfoStringGroup& flags) const
{
	QString str;
	QTextStream oss(&str);
	
	if (flags & Name)
	{
		oss << "<h2>" << name << "</h2>";
		if (!description.isEmpty())
		{
			// Let's convert possibile \n chars into <br/> in description of satellite
			oss << q_(description).replace("\n", "<br/>") << "<br/>";
		}
	}
	
	if (flags & CatalogNumber)
	{
		QString catalogNumbers;
		if (internationalDesignator.isEmpty())
			catalogNumbers = QString("%1: %2")
					 .arg(q_("Catalog #"))
					 .arg(id);
		else
			catalogNumbers = QString("%1: %2; %3: %4")
					 .arg(q_("Catalog #"))
			                 .arg(id)
					 .arg(q_("International Designator"))
			                 .arg(internationalDesignator);
		oss << catalogNumbers << "<br/><br/>";
	}

	if (flags & ObjectType)
	{
		oss << q_("Type: <b>%1</b>").arg(q_("artificial satellite")) << "<br/>";
	}
	
	if ((flags & Magnitude) && (stdMag!=99.f))
	{
		oss << q_("Approx. magnitude: <b>%1</b>").arg(QString::number(getVMagnitude(core), 'f', 2)) << "<br/>";
#ifdef IRIDIUM_SAT_TEXT_DEBUG
		oss << myText << "<br/>";
#endif
	}

	// Ra/Dec etc.
	oss << getPositionInfoString(core, flags);
	
	if (flags & Extra)
	{
		oss << "<br/>";
		// TRANSLATORS: Slant range: distance between the satellite and the observer
		oss << QString(q_("Range (km): %1")).arg(range, 5, 'f', 2);
		oss << "<br/>";
		// TRANSLATORS: Rate at which the distance changes
		oss << QString(q_("Range rate (km/s): %1")).arg(rangeRate, 5, 'f', 3);
		oss << "<br/>";
		// TRANSLATORS: Satellite altitude
		oss << QString(q_("Altitude (km): %1")).arg(height, 5, 'f', 2);
		oss << "<br/>";
		// TRANSLATORS: %1 and %3 are numbers, %2 and %4 - degree signs.
		oss << QString(q_("SubPoint (Lat./Long.): %1%2/%3%4"))
		       .arg(latLongSubPointPosition[0], 5, 'f', 2)
		       .arg(QChar(0x00B0))
		       .arg(latLongSubPointPosition[1], 5, 'f', 3)
		       .arg(QChar(0x00B0));
		oss << "<br/><br/>";
		
		//TODO: This one can be done better
		const char* xyz = "<b>X:</b> %1, <b>Y:</b> %2, <b>Z:</b> %3";
		QString temeCoords = QString(xyz)
		        .arg(position[0], 5, 'f', 2)
		        .arg(position[1], 5, 'f', 2)
		        .arg(position[2], 5, 'f', 2);
		// TRANSLATORS: TEME is an Earth-centered inertial coordinate system
		oss << QString(q_("TEME coordinates (km): %1")).arg(temeCoords);
		oss << "<br/>";
		
		QString temeVel = QString(xyz)
		        .arg(velocity[0], 5, 'f', 2)
		        .arg(velocity[1], 5, 'f', 2)
		        .arg(velocity[2], 5, 'f', 2);
		// TRANSLATORS: TEME is an Earth-centered inertial coordinate system
		oss << QString(q_("TEME velocity (km/s): %1")).arg(temeVel);
		oss << "<br/>";

		if (sunReflAngle>0)
		{  // Iridium
			oss << QString(q_("Sun reflection angle: %1%2"))
			       .arg(sunReflAngle,0,'f',1)
			       .arg(QChar(0x00B0)); // Degree sign
			oss << "<br/>";
		}
		
		//Visibility: Full text
		//TODO: Move to a more prominent place.
		switch (visibility)
		{
			case RADAR_SUN:
				oss << q_("The satellite and the observer are in sunlight.") << "<br/>";
				break;
			case VISIBLE:
				oss << q_("The satellite is visible.") << "<br/>";
				break;
			case RADAR_NIGHT:
				oss << q_("The satellite is eclipsed.") << "<br/>";
				break;
			case NOT_VISIBLE:
				oss << q_("The satellite is not visible") << "<br/>";
				break;
			default:
				break;
		}

		if (comms.size() > 0)
		{
			foreach(const CommLink &c, comms)
			{
				double dop = getDoppler(c.frequency);
				double ddop = dop;
				char sign;
				if (dop<0.)
				{
					sign='-';
					ddop*=-1;
				}
				else
					sign='+';

				oss << "<br/>";
				if (!c.modulation.isEmpty() && c.modulation != "") oss << "  " << c.modulation;
				if (!c.description.isEmpty() && c.description != "") oss << "  " << c.description;
				if ((!c.modulation.isEmpty() && c.modulation != "") || (!c.description.isEmpty() && c.description != "")) oss << "<br/>";
				oss << QString(q_("%1 MHz (%2%3 kHz)"))
				       .arg(c.frequency, 8, 'f', 5)
				       .arg(sign)
				       .arg(ddop, 6, 'f', 3);
				oss << "<br/>";
			}
		}
	}

	postProcessInfoString(str, flags);
	return str;
}

Vec3d Satellite::getJ2000EquatorialPos(const StelCore* core) const
{
	return core->altAzToJ2000(elAzPosition);;
}

Vec3f Satellite::getInfoColor(void) const
{
	return hintColor;
}

float Satellite::getVMagnitude(const StelCore* core) const
{	
	Q_UNUSED(core);
	float vmag = 7.f; // Optimistic value of magnitude for artificial satellite without data for standard magnitude
	if (!realisticModeFlag)
		vmag = 5.0;

	if (stdMag!=99.f)
	{
		sunReflAngle = -1.;
		// OK, artificial satellite has value for standard magnitude
		if (visibility==VISIBLE)
		{
			// Calculation of approx. visual magnitude for artificial satellites
			// described here: http://www.prismnet.com/~mmccants/tles/mccdesc.html
			double fracil = calculateIlluminatedFraction();
			if (fracil==0)
				fracil = 0.000001;
			if (pSatWrapper && name.startsWith("IRIDIUM"))
			{
#ifdef IRIDIUM_SAT_TEXT_DEBUG
				myText = "";
#endif
				Vec3d Sun3d = pSatWrapper->getSunECIPos();
				QVector3D sun(Sun3d.data()[0],Sun3d.data()[1],Sun3d.data()[2]);
				QVector3D sunN = sun; sunN.normalize();

#ifdef IRIDIUM_SAT_TEXT_DEBUG
				myText += "Sun3d = " + QString("[%1 %2 %3]")
						.arg(sunN.x())
						.arg(sunN.y())
						.arg(sunN.z())
						+ "<br>\n";
#endif
				//static double sin1 = sin(40*M_PI/180);
				//static double cos1 = cos(40*M_PI/180);
				//static double sin2 = sin(120*M_PI/180);
				//static double cos2 = cos(120*M_PI/180);
				// position, velocity are known
				QVector3D Vx(velocity.data()[0],velocity.data()[1],velocity.data()[2]); Vx.normalize();

#ifdef IRIDIUM_SAT_TEXT_DEBUG
				myText += "Vx = " + QString("[%1 %2 %3]")
						.arg(Vx.x())
						.arg(Vx.y())
						.arg(Vx.z())
						+ "<br>\n";
#endif
				QVector3D SatPos(position.data()[0],position.data()[1],position.data()[2]);
				Vec3d vy = (position^velocity);
				QVector3D Vy(vy.data()[0],vy.data()[1],vy.data()[2]); Vy.normalize();

#ifdef IRIDIUM_SAT_TEXT_DEBUG
				myText += "Vy = " + QString("[%1 %2 %3]")
						.arg(Vy.x())
						.arg(Vy.y())
						.arg(Vy.z())
						+ "<br>\n";
#endif
				QVector3D Vz = QVector3D::crossProduct(Vx,Vy); Vz.normalize();

#ifdef IRIDIUM_SAT_TEXT_DEBUG
				myText += "Vz = " + QString("[%1 %2 %3]")
						.arg(Vz.x())
						.arg(Vz.y())
						.arg(Vz.z())
						+ "<br>\n";
#endif

				// move this to constructor for optimizing
				QMatrix4x4 m0;
				m0.rotate(40, Vy);
				QVector3D Vx0 = m0.mapVector(Vx);
#ifdef IRIDIUM_SAT_TEXT_DEBUG
				myText += "mirror0 = " + QString("[%1 %2 %3]")
						.arg(Vx0.x())
						.arg(Vx0.y())
						.arg(Vx0.z())
						+ "<br>\n";
#endif

				QMatrix4x4 m[3];
				//m[2] = m[1] = m[0];
				m[0].rotate(0, Vz);
				m[1].rotate(120, Vz);
				m[2].rotate(-120, Vz);

				QVector3D mirror;
				sunReflAngle = 180.;

				for (int i = 0; i<3; i++)
				{
					mirror = m[i].mapVector(Vx0);
					mirror.normalize();
#ifdef IRIDIUM_SAT_TEXT_DEBUG
					myText += "mirror = " + QString("[%1 %2 %3]")
							.arg(mirror.x())
							.arg(mirror.y())
							.arg(mirror.z())
							+ "<br>\n";
#endif
					// reflection R = 2*(V dot N)*N - V
					QVector3D rsun =  2*QVector3D::dotProduct(sun,mirror)*mirror - sun;
					rsun = -rsun;
					Vec3d rSun(rsun.x(),rsun.y(),rsun.z());
#ifdef IRIDIUM_SAT_TEXT_DEBUG
					myText += "rSun = " + rSun.toString() + "<br>\n";
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////
					StelLocation loc   = StelApp::getInstance().getCore()->getCurrentLocation();
					Vec3d topoRSunPos;
					Vec3d observerECIPos;
					Vec3d observerECIVel;

					double  radLatitude    = loc.latitude * KDEG2RAD;
					double  theta          = pSatWrapper->getEpoch().toThetaLMST(loc.longitude * KDEG2RAD);

					pSatWrapper->calcObserverECIPosition(observerECIPos, observerECIVel);
#ifdef IRIDIUM_SAT_TEXT_DEBUG
					myText += "ObsPos = " + observerECIPos.toString() + " (" + observerECIPos.toStringLonLat() + ")<br>\n";
					myText += "ObsVel = " + observerECIVel.toString() + " (" + observerECIVel.toStringLonLat() + ")<br>\n";
#endif

					//Vec3d satECIPos  = getTEMEPos();
					Vec3d slantRange = rSun - observerECIPos;

					//top_s
					topoRSunPos[0] = (sin(radLatitude) * cos(theta)*slantRange[0]
							+ sin(radLatitude)* sin(theta)*slantRange[1]
							- cos(radLatitude)* slantRange[2]);
					//top_e
					topoRSunPos[1] = ((-1.0)* sin(theta)*slantRange[0]
							+ cos(theta)*slantRange[1]);

					//top_z
					topoRSunPos[2] = (cos(radLatitude) * cos(theta)*slantRange[0]
							+ cos(radLatitude) * sin(theta)*slantRange[1]
							+ sin(radLatitude) *slantRange[2]);
#ifdef IRIDIUM_SAT_TEXT_DEBUG
					myText += "SunRefl = " + topoRSunPos.toString() + " (" + topoRSunPos.toStringLonLat() + ")<br>\n";
#endif
					sunReflAngle = qMin(elAzPosition.angle(topoRSunPos) * KRAD2DEG, sunReflAngle) ;
#ifdef IRIDIUM_SAT_TEXT_DEBUG
					myText += QString("Angle = %1").arg(QString::number(sunReflAngle, 'f', 1)) + "<br>";
#endif
////////////////////////////////////////////////////////////////////////////////////////////////////


				}

				// very simple flare model
				double iridiumFlare = 100;
				if (sunReflAngle<0.5)
				{
					iridiumFlare = -8.92 + sunReflAngle*6;
				}
				else
				if (sunReflAngle<0.7)
				{
					iridiumFlare = -5.92 + (sunReflAngle-0.5)*10;
				}
					else
				{
					iridiumFlare = -3.92 + (sunReflAngle-0.7)*5;
				}


				 vmag = qMin(stdMag, iridiumFlare);
			}
			else // not Iridium
			{
				sunReflAngle = -1;
				vmag = stdMag;
			}

			vmag = vmag - 15.75 + 2.5 * std::log10(range * range / fracil);

		}
		else
			vmag = 17.f; // Artificial satellite is invisible and 17 is hypothetical value of magnitude
	}
	return vmag;
}

// Calculate illumination fraction of artifical satellite
float Satellite::calculateIlluminatedFraction() const
{
	return (1 + cos(phaseAngle))/2;
}

double Satellite::getAngularSize(const StelCore*) const
{
	return 0.00001;
}

void Satellite::setNewTleElements(const QString& tle1, const QString& tle2)
{
	if (pSatWrapper)
	{
		gSatWrapper *old = pSatWrapper;
		pSatWrapper = NULL;
		delete old;
	}

	tleElements.first.clear();
	tleElements.first.append(tle1);
	tleElements.second.clear();
	tleElements.second.append(tle2);

	pSatWrapper = new gSatWrapper(id, tle1, tle2);
	orbitPoints.clear();
	
	parseInternationalDesignator(tle1);
}

void Satellite::update(double)
{
	if (pSatWrapper && orbitValid)
	{
		StelCore* core = StelApp::getInstance().getCore();
		double JD = core->getJDay();
		epochTime = JD - core->getDeltaT(JD)/86400; // Delta T anti-correction for artificial satellites

		pSatWrapper->setEpoch(epochTime);
		position                 = pSatWrapper->getTEMEPos();
		velocity                 = pSatWrapper->getTEMEVel();
		latLongSubPointPosition  = pSatWrapper->getSubPoint();
		height                   = latLongSubPointPosition[2];
		if (height <= 0.0)
		{
			// The orbit is no longer valid.  Causes include very out of date
			// TLE, system date and time out of a reasonable range, and orbital
			// degradation and re-entry of a satellite.  In any of these cases
			// we might end up with a problem - usually a crash of Stellarium
			// because of a div/0 or something.  To prevent this, we turn off
			// the satellite.
			qWarning() << "Satellite has invalid orbit:" << name << id;
			orbitValid = false;
			displayed = false; // He shouldn't be displayed!
			return;
		}

		elAzPosition             = pSatWrapper->getAltAz();
		elAzPosition.normalize();

		pSatWrapper->getSlantRange(range, rangeRate);
		visibility = pSatWrapper->getVisibilityPredict();
		phaseAngle = pSatWrapper->getPhaseAngle();

		// Compute orbit points to draw orbit line.
		if (orbitDisplayed) computeOrbitPoints();
	}
}

double Satellite::getDoppler(double freq) const
{
	double result;
	double f = freq * 1000000;
	result = -f*((rangeRate*1000.0)/SPEED_OF_LIGHT);
	return result/1000000;
}

void Satellite::recalculateOrbitLines(void)
{
	orbitPoints.clear();
}

SatFlags Satellite::getFlags()
{
	// There's also a faster, but less readable way: treating them as uint.
	SatFlags flags;
	if (displayed)
		flags |= SatDisplayed;
	else
		flags |= SatNotDisplayed;
	if (orbitDisplayed)
		flags |= SatOrbit;
	if (userDefined)
		flags |= SatUser;
	if (newlyAdded)
		flags |= SatNew;
	if (!orbitValid)
		flags |= SatError;
	return flags;
}

void Satellite::setFlags(const SatFlags& flags)
{
	displayed = flags.testFlag(SatDisplayed);
	orbitDisplayed = flags.testFlag(SatOrbit);
	userDefined = flags.testFlag(SatUser);
}


void Satellite::parseInternationalDesignator(const QString& tle1)
{
	Q_ASSERT(!tle1.isEmpty());
	
	// The designator is encoded as columns 10-17 on the first line.
	QString rawString = tle1.mid(9, 8);
	//TODO: Use a regular expression?
	bool ok;
	int year = rawString.left(2).toInt(&ok);
	if (!rawString.isEmpty() && ok)
	{
		// Y2K bug :) I wonder what NORAD will do in 2057. :)
		if (year < 57)
			year += 2000;
		else
			year += 1900;
		internationalDesignator = QString::number(year) + "-" + rawString.right(4);
	}
	else
		year = 1957;
	
	StelUtils::getJDFromDate(&jdLaunchYearJan1, year, 1, 1, 0, 0, 0);
	//qDebug() << rawString << internationalDesignator << year;
}

bool Satellite::operator <(const Satellite& another) const
{
	// If interface strings are used, you'll need QString::localeAwareCompare()
	int comp = name.compare(another.name);
	if (comp < 0)
		return true;
	if (comp > 0)
		return false;
	
	// If the names are the same, compare IDs, i.e. NORAD numbers.
	if (id < another.id)
		return true;
	else
		return false;
}

void Satellite::draw(StelCore* core, StelPainter& painter, float)
{
	if (core->getJDay()<jdLaunchYearJan1 || !displayed)
		return;

	XYZ = getJ2000EquatorialPos(core);
	StelSkyDrawer* sd = core->getSkyDrawer();
	Vec3f drawColor(0.2f,0.2f,0.2f);
	if (visibility != RADAR_NIGHT)
		drawColor = hintColor;
	painter.setColor(drawColor[0], drawColor[1], drawColor[2], hintBrightness);

	StelProjectorP prj = core->getProjection(StelCore::FrameJ2000);

	Vec3d xy;
	if (prj->projectCheck(XYZ,xy))
	{
		if (realisticModeFlag)
		{
			double mag = getVMagnitude(core);

			RCMag rcMag;
			Vec3f color = Vec3f(1.f,1.f,1.f);

			StelProjectorP origP = painter.getProjector(); // Save projector state
			painter.setProjector(prj);

			sd->preDrawPointSource(&painter);
			if (mag <= sd->getLimitMagnitude())
			{
				sd->computeRCMag(mag, &rcMag);
				sd->drawPointSource(&painter, Vec3f(XYZ[0],XYZ[1],XYZ[2]), rcMag, color, true);
				painter.setColor(color[0], color[1], color[2], 1);

				if (Satellite::showLabels)
					painter.drawText(XYZ, name, 0, 10, 10, false);

			}

			sd->postDrawPointSource(&painter);

			painter.setProjector(origP); // Restrore projector state
		}
		else
		{
			if (Satellite::showLabels)
				painter.drawText(xy[0], xy[1], name, 0, 10, 10, false);

			Satellite::hintTexture->bind();
			painter.drawSprite2dMode(xy[0], xy[1], 11);
		}
	}
	if (orbitDisplayed && Satellite::orbitLinesFlag && orbitValid) drawOrbit(painter);
}


void Satellite::drawOrbit(StelPainter& painter)
{
	Vec3d position,previousPosition;

	glDisable(GL_TEXTURE_2D);

	QList<Vec3d>::iterator it= orbitPoints.begin();

	//First point projection calculation
	previousPosition.set(it->operator [](0), it->operator [](1), it->operator [](2));

	it++;
	StelVertexArray vertexArray;
	vertexArray.primitiveType=StelVertexArray::Lines;

	//Rest of points
	for (int i=1; i<orbitPoints.size(); i++)
	{
		position.set(it->operator [](0), it->operator [](1), it->operator [](2));
		it++;
		position.normalize();
		previousPosition.normalize();
		
		// Draw end (fading) parts of orbit lines one segment at a time.
		if (i<=orbitLineFadeSegments || orbitLineSegments-i < orbitLineFadeSegments)
		{
			painter.setColor(orbitColor[0], orbitColor[1], orbitColor[2], hintBrightness * calculateOrbitSegmentIntensity(i));
			painter.drawGreatCircleArc(previousPosition, position, &viewportHalfspace);
		}
		else
		{
			vertexArray.vertex << previousPosition << position;
		}
		previousPosition = position;
	}

	// Draw center section of orbit in one go
	painter.setColor(orbitColor[0], orbitColor[1], orbitColor[2], hintBrightness);
	painter.drawGreatCircleArcs(vertexArray, &viewportHalfspace);

	glEnable(GL_TEXTURE_2D);
}



float Satellite::calculateOrbitSegmentIntensity(int segNum)
{
	int endDist = (orbitLineSegments/2) - abs(segNum-1 - (orbitLineSegments/2) % orbitLineSegments);
	if (endDist > orbitLineFadeSegments)
	{
		return 1.0;
	}
	else
	{
		return (endDist  + 1) / (orbitLineFadeSegments + 1.0);
	}
}

void Satellite::computeOrbitPoints()
{
	gTimeSpan computeInterval(0, 0, 0, orbitLineSegmentDuration);
	gTimeSpan orbitSpan(0, 0, 0, orbitLineSegments*orbitLineSegmentDuration/2);
	gTime epochTm;
	gTime epoch(epochTime);
	gTime lastEpochComp(lastEpochCompForOrbit);
	Vec3d elAzVector;
	int diffSlots;


	if (orbitPoints.isEmpty())//Setup orbitPoins
	{
		epochTm  = epoch - orbitSpan;

		for (int i=0; i<=orbitLineSegments; i++)
		{
			pSatWrapper->setEpoch(epochTm.getGmtTm());
			elAzVector  = pSatWrapper->getAltAz();
			orbitPoints.append(elAzVector);
			epochTm    += computeInterval;
		}
		lastEpochCompForOrbit = epochTime;
	}
	else if (epochTime > lastEpochCompForOrbit)
	{ // compute next orbit point when clock runs forward

		gTimeSpan diffTime = epoch - lastEpochComp;
		diffSlots          = (int)(diffTime.getDblSeconds()/orbitLineSegmentDuration);

		if (diffSlots > 0)
		{
			if (diffSlots > orbitLineSegments)
			{
				diffSlots = orbitLineSegments + 1;
				epochTm  = epoch - orbitSpan;
			}
			else
			{
				epochTm   = lastEpochComp + orbitSpan + computeInterval;
			}

			for (int i=0; i<diffSlots; i++)
			{
				//remove points at beginning of list and add points at end.
				orbitPoints.removeFirst();
				pSatWrapper->setEpoch(epochTm.getGmtTm());
				elAzVector  = pSatWrapper->getAltAz();
				orbitPoints.append(elAzVector);
				epochTm    += computeInterval;
			}

			lastEpochCompForOrbit = epochTime;
		}
	}
	else if (epochTime < lastEpochCompForOrbit)
	{ // compute next orbit point when clock runs backward
		gTimeSpan diffTime = lastEpochComp - epoch;
		diffSlots          = (int)(diffTime.getDblSeconds()/orbitLineSegmentDuration);

		if (diffSlots > 0)
		{
			if (diffSlots > orbitLineSegments)
			{
				diffSlots = orbitLineSegments + 1;
				epochTm   = epoch + orbitSpan;
			}
			else
			{
				epochTm   = epoch - orbitSpan - computeInterval;
			}
			for (int i=0; i<diffSlots; i++)
			{ //remove points at end of list and add points at beginning.
				orbitPoints.removeLast();
				pSatWrapper->setEpoch(epochTm.getGmtTm());
				elAzVector  = pSatWrapper->getAltAz();
				orbitPoints.push_front(elAzVector);
				epochTm -= computeInterval;

			}
			lastEpochCompForOrbit = epochTime;
		}
	}
}


bool operator <(const SatelliteP& left, const SatelliteP& right)
{
	if (left.isNull())
	{
		if (right.isNull())
			return false;
		else
			return true;
	}
	if (right.isNull())
		return false; // No sense to check the left one now
	
	return ((*left) < (*right));
}
