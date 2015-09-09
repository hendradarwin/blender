/******************************************************************************
 *
 * MantaFlow fluid solver framework
 * Copyright 2011 Tobias Pfaff, Nils Thuerey
 *
 * This program is free software, distributed under the terms of the
 * GNU General Public License (GPL)
 * http://www.gnu.org/licenses
 *
 * Fire modeling plugin
 *
 ******************************************************************************/

#include "general.h"
#include "grid.h"
#include "vectorbase.h"

using namespace std;

namespace Manta {

KERNEL (bnd=1)
void KnProcessBurn(Grid<Real>& fuel,
				   Grid<Real>& density,
				   Grid<Real>& react,
				   Grid<Real>& red,
				   Grid<Real>& green,
				   Grid<Real>& blue,
				   Grid<Real>* heat,
				   Real burningRate,
				   Real flameSmoke,
				   Real ignitionTemp,
				   Real maxTemp,
				   Real dt,
				   Vec3 flameSmokeColor)
{
	// Save initial values
	Real origFuel = fuel(i,j,k);
	Real origSmoke = density(i,j,k);
	Real smokeEmit = 0.0f;
	Real flame = 0.0f;
	
	// Process fuel
	fuel(i,j,k) -= burningRate * dt;
	if (fuel(i,j,k) < 0.0f)
	{
		fuel(i,j,k) = 0.0f;
	}
	
	// Process reaction coordinate
	if (origFuel > __FLT_EPSILON__)
	{
		react(i,j,k) *= fuel(i,j,k) / origFuel;
		flame = pow(react(i,j,k), 0.5f);
	}
	else
	{
		react(i,j,k) = 0.0f;
	}
	
	// Set fluid temperature based on fuel burn rate and "flameSmoke" factor
	smokeEmit = (origFuel < 1.0f) ? (1.0 - origFuel) * 0.5f : 0.0f;
	smokeEmit = (smokeEmit + 0.5f) * (origFuel - fuel(i,j,k)) * 0.1f * flameSmoke;
	density(i,j,k) += smokeEmit;
	clamp(density(i,j,k), 0.0f, 1.0f);
	
	// Set fluid temperature from the flame temperature profile
	if (heat && flame)
	{
		(*heat)(i,j,k) = (1.0f - flame) * ignitionTemp + flame * maxTemp;
	}
	
	// Mix new color
	if (smokeEmit > __FLT_EPSILON__)
	{
		float smokeFactor = density(i,j,k) / (origSmoke + smokeEmit);
		red(i,j,k) = (red(i,j,k) + flameSmokeColor.x * smokeEmit) * smokeFactor;
		green(i,j,k) = (green(i,j,k) + flameSmokeColor.y * smokeEmit) * smokeFactor;
		blue(i,j,k) = (blue(i,j,k) + flameSmokeColor.z * smokeEmit) * smokeFactor;
	}
}

KERNEL (bnd=1)
void KnUpdateFlame(Grid<Real>& react, Grid<Real>& flame)
{
	if (react(i,j,k) > 0.0f)
		flame(i,j,k) = pow(react(i,j,k), 0.5f);
	else
		flame(i,j,k) = 0.0f;
}

PYTHON void processBurn(Grid<Real>& fuel,
						Grid<Real>& density,
						Grid<Real>& react,
						Grid<Real>& red,
						Grid<Real>& green,
						Grid<Real>& blue,
						Grid<Real>* heat = NULL,
						Real burningRate = 0.75f,
						Real flameSmoke = 1.0f,
						Real ignitionTemp = 1.25f,
						Real maxTemp = 1.75f,
						Real dt = 0.1f,
						Vec3 flameSmokeColor = Vec3(0.7f, 0.7f, 0.7f))
{
	KnProcessBurn(fuel, density, react, red, green, blue, heat, burningRate,
				  flameSmoke, ignitionTemp, maxTemp, dt, flameSmokeColor);
}

PYTHON void updateFlame(Grid<Real>& react, Grid<Real>& flame)
{
	KnUpdateFlame(react, flame);
}

} // namespace