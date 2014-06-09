/***************************************************************************
 *
 * Project:  OpenCPN Weather Routing plugin
 * Author:   Sean D'Epagnier
 *
 ***************************************************************************
 *   Copyright (C) 2014 by Sean D'Epagnier                                 *
 *   sean@depagnier.com                                                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,  USA.         *
 ***************************************************************************
 */

/* generate a datastructure which contains positions for
   isochron line segments which describe the position of the boat at a given time..

   Starting at a given location, propagate outwards in all directions.
   the outward propagation is guarenteed a closed region, and circular linked
   lists are used. If the route comes upon a boundary or reason to stop
   searching, then the point is flagged so that it is not propagated any further.

   To merge regions requires virtually the same algorithm for descrambling
   (normalizing) a single region.

   To normalize a region means that no two line segments intersect.

   For each segment go through and see if it intersects
   with any other line segment.  When it does the old route will follow
   the correct direction of the intersection on the intersected route,
   and the new region generated will be recursively normalized and then
   merged.

   A positive intersection comes in from the right.  Negative intersections
   signal negative regions.

   For each segment in a given route
   If the intersection occurs with the route and itself
   a new region is created with the same sign as the intersection
   and added to the list of either positive or negative subregions
   otherwise if the intersection occurs on different routes
   the intersecting route is merged into this one,
   swapping their connections

   Once we reach the end of the route, we can declare that it is complete,
   so in turn recursively normalize each inner subroute.  The subregions
   with the same sign are inner routes.  Once these regions are all normalized,
   the remaining regions with a different sign are the perminent subregions.
   Any inner routes remaining with matching sign can be discarded.
   
   Any outer subregions are also normalized to give outer regions
   with both signs which can be appended to the incomming lists

   Any remaining routes should be tested to ensure they are outside this one,
   Any inside routes may be discarded leaving only inverted subroutes
*/

#include <wx/wx.h>

#include "ocpn_plugin.h"
#include "GribRecordSet.h"

#include "Utilities.h"
#include "Boat.h"
#include "RouteMap.h"

#include "georef.h"

#define distance(X, Y) sqrt((X)*(X) + (Y)*(Y)) // much faster than hypot

static double Swell(GribRecordSet *grib, double lat, double lon)
{
    if(!grib)
        return 0;

    GribRecord *grh = grib->m_GribRecordPtrArray[Idx_HTSIGW];
    if(!grh)
        return 0;

    double height = grh->getInterpolatedValue(lon, lat, true );
    if(height == GRIB_NOTDEF)
        return 0;
    return height;
}

static inline bool GribWind(GribRecordSet *grib, double lat, double lon,
                            double &WG, double &VWG)
{
    if(!grib)
        return false;

    GribRecord *grx = grib->m_GribRecordPtrArray[Idx_WIND_VX];
    GribRecord *gry = grib->m_GribRecordPtrArray[Idx_WIND_VY];

    if(!grx || !gry)
        return false;

    double vx = grx->getInterpolatedValue(lon, lat, true);
    double vy = gry->getInterpolatedValue(lon, lat, true);

    if(vx == GRIB_NOTDEF || vy == GRIB_NOTDEF)
        return false;

    WG = positive_degrees(rad2deg(atan2(-vx, -vy)));
    VWG = distance(vy, vx) * 3.6 / 1.852;
    return true;
}

enum {WIND, CURRENT};

/* Compute wind speed/direction from grib or climatology */
static bool Wind(GribRecordSet *grib,
                 RouteMapConfiguration::ClimatologyDataType climatology,
                 const wxDateTime &time, bool AllowDataDeficient,
                 Position *p, double &WG, double &VWG)
{
    do {
        if(grib && GribWind(grib, p->lat, p->lon, WG, VWG))
            return true;

        if(climatology != RouteMapConfiguration::DISABLED &&
           RouteMap::ClimatologyData(WIND, time, p->lat, p->lon, WG, VWG)) {
            WG = heading_resolve(WG);
            return true;
        }

        p = p->parent;
    } while(AllowDataDeficient && p);
    return false;
}

static inline bool GribCurrent(GribRecordSet *grib, double lat, double lon,
                               double &C, double &VC)
{
    if(!grib)
        return false;

    GribRecord *grx = grib->m_GribRecordPtrArray[Idx_SEACURRENT_VX];
    GribRecord *gry = grib->m_GribRecordPtrArray[Idx_SEACURRENT_VY];

    if(!grx || !gry)
        return false;

    double vx = grx->getInterpolatedValue(lon, lat, true);
    double vy = grx->getInterpolatedValue(lon, lat, true);

    if(vx == GRIB_NOTDEF || vy == GRIB_NOTDEF)
        return false;

    C = positive_degrees(rad2deg(atan2(-vx, -vy)));
    VC = distance(vy, vx) * 3.6 / 1.852;
    return true;
}

static bool Current(GribRecordSet *grib, RouteMapConfiguration::ClimatologyDataType climatology,
                    const wxDateTime &time, double lat, double lon,
                    double &C, double &VC)
{
    if(GribCurrent(grib, lat, lon, C, VC))
        return true;

    if(climatology != RouteMapConfiguration::DISABLED &&
       RouteMap::ClimatologyData(CURRENT, time, lat, lon, C, VC))
        return true;

    return false;
}                    

/* Sometimes localized currents can be strong enough to create
   a breeze which can be sailed off even if there is no wind.
   The wind data is calculated from the ground not the sea,
   it is then converted to speed over water which the boat can feel.

   C   - Sea Current Direction over ground
   VC  - Velocity of Current
   WG  - Wind direction over ground
   VWG - Velocity of wind over ground
   WA  - Angle of wind relative to true north
   VW - velocity of wind over water
*/
static void OverWater(double WG, double VWG, double C, double VC, double &WA, double &VW)
{
    if(VC == 0) { // short-cut if no currents
        WA = WG, VW = VWG;
        return;
    }

    double Cx = VC * cos(deg2rad(C)), Cy = VC * sin(deg2rad(C));
    double Wx = VWG * cos(deg2rad(WG)) - Cx, Wy = VWG * sin(deg2rad(WG)) - Cy;
    WA = rad2deg(atan2(Wy, Wx));
    VW = distance(Wx, Wy);
}

/* provisions to compute boat movement over ground

   BG  - boat direction over ground
   BGV - boat speed over ground (gps velocity)  */
static void OverGround(double B, double VB, double C, double VC, double &BG, double &VBG)
{
    if(VC == 0) { // short-cut if no currents
        BG = B, VBG = VB;
        return;
    }

    double Cx = VC * cos(deg2rad(C)), Cy = VC * sin(deg2rad(C));
    double BGx = VB * cos(deg2rad(B)) + Cx, BGy = VB * sin(deg2rad(B)) + Cy;
    BG  = rad2deg(atan2(BGy, BGx));
    VBG = distance(BGx, BGy);
}

/* find intersection of two line segments
   if no intersection return 0, otherwise, 1 if the
   second line crosses from right to left, or -1 for left to right

   In the case that an end point basically lies on a segment, then
   -2: first point first seg
   -3: second point first seg
   2: first point second seg
   3: second point second seg

   Truth equations to calculate intersection (x, y)
   (y-y1) * (x2-x1) = (y2-y1) * (x-x1)
   (y-y3) * (x4-x3) = (y4-y3) * (x-x3)
*/
inline int TestIntersectionXY(double x1, double y1, double x2, double y2,
                              double x3, double y3, double x4, double y4)
{
    double ax = x2 - x1, ay = y2 - y1;
    double bx = x3 - x4, by = y3 - y4;
    double cx = x1 - x3, cy = y1 - y3;

    double denom = ay * bx - ax * by;

#undef EPS
#undef EPS2
#define EPS 2e-10
#define EPS2 2e-5
    if(fabs(denom) < EPS) { /* parallel or really close to parallel */
        if(fabs((y1*ax - ay*x1)*bx - (y3*bx - by*x3)*ax) > EPS2)
            return 0; /* different intercepts, no intersection */

        /* we already know from initial test we are overlapping,
           for parallel line segments, there is no way to tell
           which direction the intersection occurs */
        if(!ax && !ay) /* first segment is a zero segment */
            return -2;

        /* can invalidate a point on either segment for overlapping parallel,
           we will always choose second segment */
        double dx = x2 - x3, dy = y2 - y3;
        double da = ax*ax + bx*bx, db = cx*cx + cy*cy, dc = dx*dx + dy*dy;
        if(db <= da && dc <= da) /* point 3 is between 1 and 2 */
            return 2; 
        return 3;
    }

    double recip = 1 / denom;
    double na = (by * cx - bx * cy) * recip;
    if(na < -EPS2 || na > 1 + EPS2)
        return 0;

    double nb = (ax * cy - ay * cx) * recip;
    if(nb < -EPS2 || nb > 1 + EPS2)
        return 0;

    /* too close to call.. floating point loses bits with arithmetic so
       in this case we must avoid potential false guesses */
    if(na < EPS2)     return -2;
    if(na > 1 - EPS2) return -3;
    if(nb < EPS2)     return 2;
    if(nb > 1 - EPS2) return 3;

    return denom < 0 ? -1 : 1;
}

#define EPSILON (2e-10)
Position::Position(double latitude, double longitude, Position *p, int sp, int t, int u, int pr)
    : lat(latitude), lon(longitude), sailplan(sp), tacks(t),
      upwind(u), propagations(pr),
      parent(p), propagated(false), copied(false)
{
    lat -= fmod(lat, EPSILON);
    lon -= fmod(lon, EPSILON);
}

Position::Position(Position *p)
    : lat(p->lat), lon(p->lon), sailplan(p->sailplan), tacks(p->tacks),
      upwind(p->upwind), propagations(p->propagations),
      parent(p->parent), propagated(p->propagated), copied(true)
{
}

/* sufficient for routemap uses only.. is this faster than below? if not, remove it */
static int ComputeQuadrantFast(Position *p, Position *q)
{
    int quadrant;
    if(q->lat < p->lat)
        quadrant = 0;
    else
        quadrant = 2;

    if(p->lon  < q->lon)
        quadrant++;

    return quadrant;
}

/* works for all ranges */
static int ComputeQuadrant(Position *p, Position *q)
{
    int quadrant;
    if(q->lat < p->lat)
        quadrant = 0;
    else
        quadrant = 2;

    double diff = p->lon - q->lon;
    while(diff < -180) diff += 360;
    while(diff >= 180) diff -= 360;
    
    if(diff < 0)
        quadrant++;

    return quadrant;
}

SkipPosition *Position::BuildSkipList()
{
    /* build skip list of positions, skipping over strings of positions in
       the same quadrant */
    SkipPosition *skippoints = NULL;
    Position *p = this;
    int firstquadrant, lastquadrant = -1, quadrant;
    do {
        Position *q = p->next;
        quadrant = ComputeQuadrantFast(p, q);

        if(lastquadrant == -1)
            firstquadrant = lastquadrant = quadrant;
        else
        if(quadrant != lastquadrant) {
            SkipPosition *rs = new SkipPosition(p, quadrant);
            if(skippoints) {
                rs->prev=skippoints->prev;
                rs->next=skippoints;
                skippoints->prev->next = rs;
                skippoints->prev = rs;
            } else {
                skippoints = rs;
                rs->prev = rs->next = rs;
            }
            lastquadrant = quadrant;
        }
        p = q;
    } while(p != this);

    if(!skippoints) {
        SkipPosition *rs = new SkipPosition(p, quadrant);
        rs->prev = rs->next = rs;
        skippoints = rs;
    } else
    if(quadrant != firstquadrant) {
        SkipPosition *rs = new SkipPosition(p, firstquadrant);

        rs->prev=skippoints->prev;
        rs->next=skippoints;
        skippoints->prev->next = rs;
        skippoints->prev = rs;

        skippoints = rs;
    }
    return skippoints;
}

struct climatology_wind_atlas
{
    double W[8], VW[8], storm, calm, directions[8];
};

static inline bool ReadWindAndCurrents
(GribRecordSet *grib, const wxDateTime &time, RouteMapConfiguration &configuration, Position *p,
/* normal data */
 double &WG, double &VWG, double &W, double &VW, double &C, double &VC,
 climatology_wind_atlas &atlas)
{
    /* read current data */
    if(!configuration.Currents || !Current(grib, configuration.ClimatologyType,
                                           time, p->lat, p->lon, C, VC))
        C = VC = 0;

    int windatlas_count = 8;
    if(grib || configuration.ClimatologyType == RouteMapConfiguration::AVERAGE) {
        if(!Wind(grib, configuration.ClimatologyType, time,
                 configuration.AllowDataDeficient, p, WG, VWG))
        return false;
    } else if(configuration.ClimatologyType != RouteMapConfiguration::DISABLED) {
        double speeds[8];
        while(!RouteMap::ClimatologyWindAtlasData(time, p->lat, p->lon, windatlas_count,
                                                  atlas.directions, speeds, atlas.storm, atlas.calm)) {
            if(!configuration.AllowDataDeficient)
                return false;
            p = p->parent;
            if(!p)
                return false;
        }

        /* compute wind speeds over water with the given current */
        for(int i=0; i<windatlas_count; i++) {
            double WG = i*360/windatlas_count;
            double VWG = speeds[i];
            OverWater(WG, VWG, C, VC, atlas.W[i], atlas.VW[i]);
        }

        /* find most likely wind direction */
        double max_direction = 0;
        int maxi;
        for(int i=0; i<windatlas_count; i++)
            if(atlas.directions[i] > max_direction) {
                max_direction = atlas.directions[i];
                maxi = i;
            }
        
        /* now compute next most likely wind octant (adjacent to most likely) and
           linearly interpolate speed and direction from these two octants,
           we use this as the most likely wind, and base wind direction for the map */
        int maxia = maxi+1, maxib = maxi-1;
        if(maxia == windatlas_count)
            maxia = 0;
        if(maxib < 0)
            maxib = windatlas_count - 1;

        if(atlas.directions[maxia] < atlas.directions[maxib])
            maxia = maxib;

        double maxid = 1 / (atlas.directions[maxi] / atlas.directions[maxia] + 1);
        double angle1 = atlas.W[maxia], angle2 = atlas.W[maxi];
        while(angle1 - angle2 > 180) angle1 -= 360;
        while(angle2 - angle1 > 180) angle2 -= 360;
        W = positive_degrees(maxid*angle1 + (1-maxid)*angle2);
        VW = maxid*atlas.VW[maxia] + (1-maxid)*atlas.VW[maxi];
        
        OverGround(W, VW, C, VC, WG, VWG);
        return true;
    }

    OverWater(WG, VWG, C, VC, W, VW);
    return true;
}

/* get data from a position for plotting */
bool Position::GetPlotData(GribRecordSet *grib, double dt,
                           RouteMapConfiguration &configuration, PlotData &data)
{
    data.WVHT = Swell(grib, lat, lon);

    climatology_wind_atlas atlas;
    ReadWindAndCurrents(grib, data.time, configuration, this,
                        data.WG, data.VWG, data.W, data.VW, data.C, data.VC, atlas);

    if(parent) {
        ll_gc_ll_reverse(parent->lat, parent->lon, lat, lon, &data.BG, &data.VBG);
        if(dt == 0)
            data.VBG = 0;
        else
            data.VBG *= 3600 / dt;
        OverWater(data.BG, data.VBG, data.C, data.VC, data.B, data.VB);
        return true;
    }

    return false;
}

static inline bool ComputeBoatSpeed
(RouteMapConfiguration &configuration, double timeseconds,
 double WG, double VWG, double W, double VW, double C, double VC, double &H,
 climatology_wind_atlas &atlas,
 double &B, double &VB, double &BG, double &VBG, double &dist, int newsailplan)
{
    if(configuration.ClimatologyType == RouteMapConfiguration::CUMULATIVE_MAP ||
       configuration.ClimatologyType == RouteMapConfiguration::CUMULATIVE_MINUS_CALMS) {
        /* build map */
        VB = 0;
        int windatlas_count = 8;
        for(int i = 0; i<windatlas_count; i++) {
            double VBc = configuration.boat.Plans[newsailplan].Speed(H-W+atlas.W[i], atlas.VW[i]);
            VB += atlas.directions[i]*VBc;
        }

        if(configuration.ClimatologyType == RouteMapConfiguration::CUMULATIVE_MINUS_CALMS)
            VB *= 1-atlas.calm;
    } else
        VB = configuration.boat.Plans[newsailplan].Speed(H, VW);

    /* failed to determine speed.. */
    if(isnan(B) || isnan(VB))
        return false; //B = VB = 0;

    /* compound boatspeed with current */
    OverGround(B, VB, C, VC, BG, VBG);

    if(!VBG) // no speed
        return false;

    /* distance over ground */
    dist = VBG * timeseconds / 3600.0;
    
    return true;
}

bool rk_step(Position *p, double timeseconds, double BG, double dist, double H,
             RouteMapConfiguration &configuration, GribRecordSet *grib,
             const wxDateTime &time, int newsailplan,
             double &rk_BG, double &rk_dist)
{
    double k1_lat, k1_lon;
    ll_gc_ll(p->lat, p->lon, BG, dist, &k1_lat, &k1_lon);

    double WG, VWG, W, VW, C, VC;
    climatology_wind_atlas atlas;
    Position rk(k1_lat, k1_lon, p->parent); // parent so deficient data can find parent
    if(!ReadWindAndCurrents(grib, time, configuration, &rk,
                            WG, VWG, W, VW, C, VC, atlas))
        return false;

    double B = W + H; /* rotated relative to true wind */

    double VB, VBG; // outputs
    if(!ComputeBoatSpeed(configuration, timeseconds, WG, VWG, W, VW, C, VC, H, atlas,
                         B, VB, rk_BG, VBG, rk_dist, newsailplan))
        return false;

    return true;
}

inline double TestDirection(double x1, double y1, double x2, double y2, double x3, double y3)
{
    double ax = x2 - x1, ay = y2 - y1;
    double bx = x2 - x3, by = y2 - y3;
    double denom = ay * bx - ax * by;
    return denom;
}

void DeletePoints(Position *point)
{
    Position *p = point;
    do {
        Position *dp = p;
        p = p->next;
        delete dp;
    } while(p != point);
}

/* create a looped route by propagating from a position by computing
   the location the boat would be in if sailed at various angles */
bool Position::Propagate(IsoRouteList &routelist, GribRecordSet *grib,
                         const wxDateTime &time, RouteMapConfiguration &configuration)
{
    /* already propagated from this position, don't need to again */
    if(propagated)
        return false;

    propagated = true;

    Position *points = NULL;
    /* through all angles relative to wind */
    int count = 0;

    double S = Swell(grib, lat, lon);
    if(S > configuration.MaxSwellMeters)
        return false;

    if(fabs(lat) > configuration.MaxLatitude)
        return false;

    double WG, VWG, W, VW, C, VC;
    climatology_wind_atlas atlas;
    if(!ReadWindAndCurrents(grib, time, configuration, this,
                            WG, VWG, W, VW, C, VC, atlas))
        return false;

    if(VW > configuration.MaxWindKnots)
        return false;

    int daytime = -1; /* unknown */

    double parentbearing = NAN;
    /* compute parent bearing if needed;
       the tack should probably be stored as a flag in the position class instead */
    if(parent && (configuration.TackingTime || configuration.MaxTacks>=0))
        ll_gc_ll_reverse(parent->lat, parent->lon, lat, lon, &parentbearing, 0);

    double timeseconds = configuration.dt;
    double dist;

    bool tacked = false;
    int cur_upwind = 0;

    int propagation_direction = 1, propagation_skips = 0;
    std::list<double>::iterator it = configuration.DegreeSteps.begin(), orig_end = configuration.DegreeSteps.end();
    it--;
    orig_end--;
    std::list<double>::iterator       end = orig_end;

    for(;;) {
        if(propagation_direction==1) it++; else it--;
        if(it == configuration.DegreeSteps.end()) /* skip past end node in circular linked list */
        {  if(propagation_direction==1) it++; else it--; }
        if(it == end)
            break;
    skipin:

        double H = *it;
        double B, VB, BG, VBG;

        int newsailplan = configuration.boat.TrySwitchBoatPlan(sailplan, VW, H, S,
                                                               time, lat, lon, daytime);
        B = W + H; /* rotated relative to true wind */

        if(!ComputeBoatSpeed(configuration, timeseconds, WG, VWG, W, VW, C, VC, H, atlas,
                             B, VB, BG, VBG, dist, newsailplan))
            continue;

        /* perform upwind percentage calculation relative to apparent wind,
           techically can be done before computing boat over ground speed to bail sooner,
           but this is simpler logically */
        if(configuration.MaxUpwindPercentage < 100) {
            double VA = BoatPlan::VelocityApparentWind(VB, deg2rad(H), VW);
            double A = rad2deg(BoatPlan::DirectionApparentWind(VA, VB, deg2rad(H), VW));

            if(fabs(A) < 90) { /* use 90 degrees.. maybe we can allow the user to set this */
                if(100.0 * (upwind + 1) / (propagations + 1) > configuration.MaxUpwindPercentage)
                    continue;
                cur_upwind = 1;
            }
        }

        /* did we tack? apply penalty */
        if(!isnan(parentbearing)) {
            double hrpb = heading_resolve(parentbearing), hrb = B;
            if(hrpb*hrb < 0 && fabs(hrpb - hrb) < 180) {
                timeseconds -= configuration.TackingTime;

                if(configuration.MaxTacks >= 0 && tacks >= configuration.MaxTacks)
                    continue;
                tacked = true;
            }
        }

        double dlat, dlon, nrdlon;
        if(configuration.Integrator == RouteMapConfiguration::RUNGE_KUTTA) {
            double k2_dist, k2_BG, k3_dist, k3_BG, k4_dist, k4_BG;
            // a lot more experimentation is needed here
#if 0
            if(!rk_step(p, timeseconds,    BG,    dist/2, H,
                        configuration, grib, time, newsailplan, k2_BG, k2_dist) ||
               !rk_step(p, timeseconds, k2_BG, k2_dist/2, H,
                        configuration, grib, time, newsailplan, k3_BG, k3_dist) ||
               !rk_step(p, timeseconds, k3_BG, k3_dist,   H,
                        configuration, grib, time, newsailplan, k4_BG, k4_dist))
                continue;
            ll_gc_ll(lat,  lon,     BG,    dist/6, &dlat, &dlon);
            ll_gc_ll(dlat, dlon, k2_BG, k2_dist/3, &dlat, &dlon);
            ll_gc_ll(dlat, dlon, k3_BG, k3_dist/3, &dlat, &dlon);
            ll_gc_ll(dlat, dlon, k4_BG, k4_dist/6, &dlat, &dlon);
#else
            wxDateTime rk_time_2 = time + wxTimeSpan::Seconds(timeseconds/2);
            wxDateTime rk_time = time + wxTimeSpan::Seconds(timeseconds);
            if(!rk_step(this, timeseconds, BG,    dist/2, H,
                        configuration, grib, rk_time_2, newsailplan, k2_BG, k2_dist) ||
               !rk_step(this, timeseconds, BG, k2_dist/2, H + k2_BG - BG,
                        configuration, grib, rk_time_2, newsailplan, k3_BG, k3_dist) ||
               !rk_step(this, timeseconds, BG, k3_dist,   H + k3_BG - BG,
                        configuration, grib, rk_time, newsailplan, k4_BG, k4_dist))
                continue;

            ll_gc_ll(lat, lon, BG, dist/6 + k2_dist/3 + k3_dist/3 + k4_dist/6, &dlat, &dlon);
#endif
        } else /* newtons method */
            ll_gc_ll(lat, lon, BG, dist, &dlat, &dlon);

        bool current_propagated = false;
#if 0 // unfortunately still bugged
        /* test to avoid extra computations related to backtracking,
           this doesn't help a lot, but if we could stop, and reverse until
           failing again, then we would avoid a lot of calculations.  If the
           first test fails, simply skip to the opposite H and start from
           there */
        if(prev != next && parent) {
            if(!current_propagated) {

                double d0, d1, d2;
                d0 = TestDirection(prev->lat, prev->lon, lat, lon, next->lat, next->lon);
                d1 = TestDirection(prev->lat, prev->lon, lat, lon, dlat, dlon);
                d2 = TestDirection(dlat, dlon, lat, lon, next->lat, next->lon);
                
                current_propagated = ((d1 > 0 && d2 > 0) || (d0 < 0 && (d1 > 0 || d2 > 0)));
            }
                
            if(current_propagated) {
                if(propagation_direction == 1) {
                    if (count) {
                        if(!points->prev->propagated) {
                           propagation_direction = -2;
                           end = it;
                           it = orig_end;
                        }
                    } else { // initial point was a miss.. skip ahead and try there?
                        if(propagation_skips < 2) {
                            for(unsigned int skip = 0; skip < configuration.DegreeSteps.size()/3; skip++)
                                it++;
                            if(it == configuration.DegreeSteps.end())
                                it++;

                            orig_end = end = it;
                            propagation_skips++;
                            goto skipin;
                        }
                    }
                } else {
                    it = end; // finished after this one
                    it++;
                }
            }
        }
#endif  

        nrdlon = dlon;
        if(configuration.positive_longitudes && dlon < 0)
            dlon += 360;

        if(configuration.MaxSearchAngle < 180) {
            double bearing;
            ll_gc_ll_reverse(configuration.StartLat, configuration.StartLon, dlat, dlon, &bearing, 0);

            if(fabs(heading_resolve(configuration.StartEndBearing - bearing)) > configuration.MaxSearchAngle)
                continue;
        }

        if(configuration.MaxDivertedCourse < 180) {
            double bearing, dist;
            ll_gc_ll_reverse(dlat, dlon, configuration.EndLat, configuration.EndLon, &bearing, &dist);

            double bearing1, dist1;
            ll_gc_ll_reverse(configuration.StartLat, configuration.StartLon, dlat, dlon, &bearing1, &dist1);

            double term = (dist1 + dist) / dist;
            term = pow(term/16, 4) + 1; // make 1 until the end, then make big

            if(fabs(heading_resolve(bearing1 - bearing)) > configuration.MaxDivertedCourse * term)
                continue;
        }

        /* landfall test */
        if(configuration.DetectLand && CrossesLand(dlat, nrdlon))
            continue;

        /* crosses cyclone track(s)? */
        if(configuration.AvoidCycloneTracks) {
            int crossings = RouteMap::ClimatologyCycloneTrackCrossings
                (lat, lon, dlat, dlon, time, configuration.CycloneMonths*30 +
                 configuration.CycloneDays, configuration.CycloneWindSpeed,
                 wxDateTime(0, 0, configuration.CycloneClimatologyStartYear));
            if(crossings > 0)
                continue;
        }

        Position *rp = new Position(dlat, dlon, this, newsailplan, tacks + tacked,
                                    upwind + cur_upwind, propagations + 1);

        rp->propagated = current_propagated;

        if(points) {
            rp->prev=points->prev;
            rp->next=points;
            points->prev->next = rp;
            points->prev = rp;
            
            if(propagation_direction == -1)
                points = points->prev;
            if(propagation_direction == -2)
                propagation_direction = -1;
        } else {
            rp->prev = rp->next = rp;
            points = rp;
        }
        count++;
    }

#if 1 // remove strings that are already propagated */
reset:
    Position *p = points;
    while(count >= 3) {
        Position *pn = p->next;
        if(p->prev->propagated && p->propagated && p->next->propagated) {
            p->prev->next = p->next;
            p->next->prev = p->prev;
            delete p;
            count--;
            if(p == points) {
                points = pn;
                goto reset;
            }
        }
        p = pn;
        if(p == points)
            break;
    }
#endif

    if(count < 3) { /* would get eliminated anyway, but save the extra steps */
        if(count)
            DeletePoints(points);
        return false;
    }

    IsoRoute *nr = new IsoRoute(points->BuildSkipList());
#if 1
    routelist.push_back(nr);
#else
    bool Normalize(IsoRouteList &rl, IsoRoute *route1, IsoRoute *route2, int level, bool inverted_regions);
    Normalize(routelist, nr, nr, 0, configuration.InvertedRegions);
#endif
    return true;
}

wxDateTime RouteMap::EndDate()
{
    IsoChronList::iterator it = origin.end();
    if(it == origin.begin())
        return wxDateTime();
    it--;

    double dista;
    wxDateTime timea = (*it)->time;
    Position *a = (*it)->ClosestPosition(m_Configuration.EndLat, m_Configuration.EndLon, &dista);
    if(!a || it == origin.begin())
        return wxDateTime();

    it--;
    double distb;
    wxDateTime timeb = (*it)->time;
    Position *b = (*it)->ClosestPosition(m_Configuration.EndLat, m_Configuration.EndLon, &distb);
    if(!b)
        return wxDateTime();

    double seconds = (timea - timeb).GetSeconds().ToLong();
    wxTimeSpan span_after_b = wxTimeSpan::Seconds(seconds * distb / (dista + distb));
    return timeb + span_after_b;
}

double Position::Distance(Position *p)
{
    return DistGreatCircle(lat, lon, p->lat, p->lon);
}

bool Position::CrossesLand(double dlat, double dlon)
{
    return PlugIn_GSHHS_CrossesLand(lat, lon, dlat, dlon);
}

SkipPosition::SkipPosition(Position *p, int q)
 : point(p), quadrant(q)
{
}

void SkipPosition::Remove()
{
    prev->next = next;
    next->prev = prev;
    delete this;
}

/* copy a skip list along with it's position list to new lists */
SkipPosition* SkipPosition::Copy()
{
    SkipPosition *s = this;
    if(!s)
        return s;

    SkipPosition *fs, *ns = NULL;
    Position *fp, *np = NULL;
    Position *p = s->point;
    do {
        Position *nsp = NULL;
        do { /* copy all positions between skip positions */
            Position *nnp = new Position(p);
            if(!nsp)
                nsp = nnp;
            if(np) {
                np->next = nnp;
                nnp->prev = np;
                np = nnp;
            } else {
                fp = np = nnp;
                np->prev = np->next = np;
            }
            p = p->next;
        } while(p != s->next->point);

        SkipPosition *nns = new SkipPosition(nsp, s->quadrant);
        if(ns) {
            ns->next = nns;
            nns->prev = ns;
            ns = nns;
        } else {
            fs = ns = nns;
            ns->prev = ns->next = nns;
        }
        s = s->next;
    } while(s != this);

    fs->prev = ns;
    ns->next = fs;

    fp->prev = np;
    np->next = fp;
    return fs;
}

void DeleteSkipPoints(SkipPosition *skippoints)
{
    SkipPosition *s = skippoints;
    do {
        SkipPosition *ds = s;
        s = s->next;
        delete ds;
    } while(s != skippoints);
}

IsoRoute::IsoRoute(SkipPosition *s, int dir)
    : skippoints(s), direction(dir), parent(NULL)
{
}

/* copy constructor */
IsoRoute::IsoRoute(IsoRoute *r, IsoRoute *p)
    : skippoints(r->skippoints->Copy()), direction(r->direction), parent(p)
{
}

IsoRoute::~IsoRoute()
{
    for(IsoRouteList::iterator it = children.begin(); it != children.end(); ++it)
        delete *it;

    if(!skippoints)
        return;

    DeletePoints(skippoints->point);
    DeleteSkipPoints(skippoints);
}

void IsoRoute::Print()
{
    if(!skippoints)
        printf("Empty IsoRoute\n");
    else {
        Position *p = skippoints->point;
        do {
            printf("%.8f %.8f\n", p->lon, p->lat);
            p = p->next;
        } while(p != skippoints->point);
        printf("\n");
    }
}

void IsoRoute::PrintSkip()
{
    if(!skippoints)
        printf("Empty IsoRoute\n");
    else {
        SkipPosition *s = skippoints;
        do {
            printf("%.4f %.4f\n", s->point->lon, s->point->lat);
            s = s->next;
        } while(s != skippoints);
        printf("\n");
    }
}

/* how many times do we cross this route going from this point to infinity,
   return -1 if inconclusive */
int IsoRoute::IntersectionCount(Position &pos)
{
    int numintsct = 0;
    double lat = pos.lat, lon = pos.lon;

    SkipPosition *s1 = skippoints;
    do {
        SkipPosition *s2 = s1->next;
        double s1plon = s1->point->lon, s2plon = s2->point->lon;

        /* we could avoid completely recomputing the state every time,
           and use all 9 states, cutting comparison tests and switches
           in half (nearly double the speed of this routine)
           but it would complicate things, and this routine tends to
           take less than 1% of the total runtime */
        int state = (lon < s1plon) + (lon < s2plon);
        if(state == 1) {
            double s1plat = s1->point->lat, s2plat = s2->point->lat;
            state = (lat < s1plat) + (lat < s2plat);

            switch(state) {
            case 1: /* must test every point in this case as point falls in boundaries of skip list */
            {
                Position *p1 = s1->point;
                do {
                    Position *p2 = p1->next;
                    double p1lon = p1->lon, p2lon = p2->lon;
                    state = (lon < p1lon) + (lon < p2lon);
                    if(state == 1) {
                        double p1lat = p1->lat, p2lat = p2->lat;
                        state = (lat < p1lat) + (lat < p2lat);
                        switch(state) {
                        case 1: /* must perform exact intersection test */
                        {
                            int dir = TestIntersectionXY(p1lon, p1lat, p2lon, p2lat, lon, lat, lon, 91);
                            switch(dir) {
                            case -2: case -3: case 2: case 3: return -1;
                            case 1: case -1: goto intersects;
                            }
                        } break;
                        case 2: /* must intersect, we are below */
                            goto intersects;
                        }
                    }
                    p1 = p2;
                } while(p1 != s2->point);
            } break;
            case 2: /* must intersect, we are below skip segment out of bounds */
            intersects:
                numintsct++;
            }
        }

        s1 = s2;
    } while(s1 != skippoints);

    return numintsct;
}

/* determine if a route contains a position
   0 for outside, 1 for inside, -1 for inconclusive (on border or really close) */
int IsoRoute::Contains(Position &pos, bool test_children)
{
    int numintsct = IntersectionCount(pos);
    if(numintsct == -1)
        return -1;

    if(test_children)
        for(IsoRouteList::iterator it = children.begin();
            it != children.end(); it++) {
            int cnumintsct = (*it)->Contains(pos, test_children);
            if(cnumintsct == -1)
                return -1;
            numintsct += cnumintsct;
        }

    return numintsct&1; /* odd */
}

/* This function is very slow, and should probably be removed
   or replaced with something else.. see how often it is called */
bool IsoRoute::CompletelyContained(IsoRoute *r)
{
    Position *pos = r->skippoints->point;
    do {
        if(Contains(*pos, false) != 1)
            return false;
        pos = pos->next;
    } while(pos != r->skippoints->point);
    return true;
}

/* Determine if one route contains another,
   only test first point, but if it fails try other points */
bool IsoRoute::ContainsRoute(IsoRoute *r)
{
    Position *pos = r->skippoints->point;
    do {
        switch(Contains(*pos, false)) {
        case 0: return false;
        case 1: return true;
        }

        pos = pos->next;
    } while(pos != r->skippoints->point); /* avoid deadlock.. lets hope we dont do this often */

    printf("bad contains route\n");
    return true; /* probably good to say it is contained in this unlikely case */
}

/* apply current to given route, and return if it changed at all */
#if 0
bool IsoRoute::ApplyCurrents(GribRecordSet *grib, wxDateTime time, RouteMapConfiguration &configuration)
{
    if(!skippoints)
        return false;

    bool ret = false;
    Position *p = skippoints->point;
    double timeseconds = configuration.dt;
    do {
        double C, VC;
        if(configuration.Currents && Current(grib, configuration.ClimatologyType,
                                             time, p->lat, p->lon, C, VC)) {
            /* drift distance over ground */
            double dist = VC * timeseconds / 3600.0;
            if(dist)
                ret = true;
            ll_gc_ll(p->lat, p->lon, C, dist, &p->lat, &p->lon);
        }

        p = p->next;
    } while(p != skippoints->point);

    /* if we moved we need to rebuild the skip list */
    if(ret) {
        Position *points = skippoints->point;
        DeleteSkipPoints(skippoints);
        skippoints = points->BuildSkipList();
    }

    return ret;
}
#endif

enum { MINLON, MAXLON, MINLAT, MAXLAT };
/* return false if longitude is possibly invalid
   could cache these bounds to avoid recomputing all the time */
void IsoRoute::FindIsoRouteBounds(double bounds[4])
{
    SkipPosition *maxlat = skippoints;
    Position *p = skippoints->point;
    bounds[MINLAT] = bounds[MAXLAT] = p->lat;
    bounds[MINLON] = bounds[MAXLON] = p->lon;

    SkipPosition *s = skippoints->next;
    while(s != skippoints) {
        p = s->point;
        bounds[MINLAT] = MIN(p->lat, bounds[MINLAT]);
        bounds[MAXLAT] = MAX(p->lat, bounds[MAXLAT]);
        bounds[MINLON] = MIN(p->lon, bounds[MINLON]);
        bounds[MAXLON] = MAX(p->lon, bounds[MAXLON]);
            
        if(p->lat == bounds[MAXLAT])
            maxlat = s;
        s = s->next;
    }
    skippoints = maxlat; /* set to max lat for merging to keep outside */
}

bool checkskiplist(SkipPosition *s)
{
    /* build skip list of positions, skipping over strings of positions in
       the same quadrant */
    SkipPosition *skippoints = s;
    Position *p = s->point;
    do {
        do {
            Position *q = p->next;
            int quadrant = ComputeQuadrantFast(p, q);

            if(quadrant != s->quadrant)
                return false;
            p = q;
        } while(p != s->next->point);
        s = s->next;
    } while(s != skippoints);
    return true;
}

/* nudge a position by a miniscule amount so it can pass the intersection test */
void IsoRoute::PerturbPosition(SkipPosition *s, Position *p)
{
    if(s->next->point == p)
        s = s->next;

    double epsilon = 2e-7;
    if(s->point == p) {
        switch(s->quadrant + 4*s->prev->quadrant) {
        case 1: case 3:
        case 9:  case 11: p->lon -= epsilon; break;
        case 4: case 6:
        case 12: case 14: p->lon += epsilon; break;
        case 2: case 7:   p->lat -= epsilon; break;
        case 8: case 13:  p->lat += epsilon; break;
        default: printf("invalid skip list\n");
        }
    } else {
        switch(s->quadrant) {
        case 0: case 2: p->lon -= epsilon;
            if(p->lon >= p->next->lon) return;
            break;
        case 1: case 3: p->lon += epsilon;
            if(p->lon <= p->next->lon) return;
            break;
        }

        printf("perturb traffic jam\n");
#if 0
        PerturbPosition(s, p->next);
#else
        /* rebuild skip list */
        Position *points = skippoints->point;
        DeleteSkipPoints(skippoints);
        skippoints = points->BuildSkipList();
#endif
    }

}

/* remove and delete a position given it's last skip position,
   we need to update the skip list if this point falls on a skip position*/
void IsoRoute::RemovePosition(SkipPosition *s, Position *p)
{
    if(s == s->next) {
        delete s->point;
        delete s;
        skippoints = NULL;
    } else {
        p->next->prev = p->prev;
        p->prev->next = p->next;
    }

    if(s->point == p) {
#if 0
    /* possible optimization to avoid rebuilding the skip list.. is this really useful? */
        /* warning: this is not correct yet */
        int pquadrant = s->prev->quadrant;
        int quadrant = FindQuadrant(p->prev, p->next);
        if(quadrant == pquadrant) {
            s->point = p->next;
            if(s->point == s->next->point || quadrant == s->quadrant) {
                s->prev->next = s->next;
                s->next->prev = s->prev;
                delete s;
            }
        } else {
            s->point = p->prev;
            if(s->point == s->prev->point || quadrant = s->next->quadrant) {
                s->next->point = s->point;
                s->prev->next = s->next;
                s->next->prev = s->prev;
                delete s;
            }
            s->quadrant = quadrant;
        }
#else
        /* rebuild skip list */
        Position *points = skippoints->point;
        if(p == points)
            points = points->next;
        DeleteSkipPoints(skippoints);
        skippoints = points->BuildSkipList();
#endif
    }
    delete p;
}

inline void SwapSegments(Position *p, Position *q, Position *r, Position *s)
{
    p->next = s;
    s->prev = p;
    r->next = q;
    q->prev = r;
}

inline void SwapSkipSegments(SkipPosition *sp, SkipPosition *sq, SkipPosition *sr, SkipPosition *ss)
{
    sp->next = ss;
    ss->prev = sp;
    sr->next = sq;
    sq->prev = sr;
}

inline void InsertSkipPosition(SkipPosition *sp, SkipPosition *sn, Position *p, int quadrant)
{
    SkipPosition *s = new SkipPosition(p, quadrant);
    s->prev = sp;
    sp->next = s;
    s->next = sn;
    sn->prev = s;
}

/* given positions p and s in skip list between sp and ss, fix stuff adding removing
   or shifting skip positions to make things valid after this merge */
/*inline*/ void FixSkipList(SkipPosition *sp, SkipPosition *ss, Position *p, Position *s,
                            int rquadrant, SkipPosition *&spend, SkipPosition *&ssend)
{
    int quadrant = ComputeQuadrantFast(p, s);
    if(sp->point == p) {
        sp->quadrant = quadrant; /* reuse p with this quadrant */

        if(quadrant == sp->prev->quadrant && sp != ss) {
            sp->point = sp->prev->point;
            if(sp->prev == spend)
                spend = sp;
            if(sp->prev == ssend)
                ssend = sp;
            if(ss == sp->prev) {
              if(ssend == ss)
                ssend = sp;
              ss = sp;
            }
            sp->prev->Remove();
        }
/* DUPLICATE START */
        if(quadrant == rquadrant) {
            if(rquadrant == ss->quadrant)
                goto remove;
        } else if(ss->point == s) {
            if(quadrant == ss->quadrant)
                goto remove;
        } else {
            if(rquadrant == ss->quadrant)
                ss->point = s; /* shift ss to s */
            else
                InsertSkipPosition(sp, ss, s, rquadrant);
        }
/* DUPLICATE END */
    } else
    if(sp->quadrant == quadrant) {
        if(quadrant == rquadrant) { /* this is never hit..  can we remove this test? */
            if(rquadrant == ss->quadrant)
                goto remove;
        } else if(ss->point == s) {
            if(quadrant == ss->quadrant) {
            remove:
              if(sp == ss)
                printf("sp == ss.. this is bad\n");
              if(ss == spend)
                  spend = ss->next;
              if(ss == ssend)
                  ssend = ss->next;
              ss->Remove();
            }
        } else {
            if(rquadrant == ss->quadrant)
                ss->point = s; /* shift ss to s */
            else
                InsertSkipPosition(sp, ss, s, rquadrant);
        }
    } else {
        if(quadrant == rquadrant) {
            if(rquadrant == ss->quadrant)
                ss->point = p; /* shift ss to p */
            else
                InsertSkipPosition(sp, ss, p, quadrant);
        } else if(ss->point == s) {
            if(quadrant == ss->quadrant)
                ss->point = p; /* shift ss to p */
            else
                InsertSkipPosition(sp, ss, p, quadrant);
        } else {
            InsertSkipPosition(sp, ss, p, quadrant);
            if(rquadrant == ss->quadrant)
                ss->point = s; /* shift ss to s */
            else
                InsertSkipPosition(sp->next, ss, s, rquadrant);
        }
    }
}

bool UpdateEnd(SkipPosition *spend, SkipPosition *sr)
{
    SkipPosition *nsr = sr;
    do {
        if(nsr == spend)
            return true;
        nsr = nsr->next;
    } while(nsr != sr);
    return false;
}

#define COMPUTE_MIN_MAX(quadrant, A, B, N)        \
      switch(quadrant) { \
      default: min##N##x = B##x; max##N##x = A##x; min##N##y = B##y, max##N##y = A##y; break; \
      case 1:  min##N##x = A##x; max##N##x = B##x; min##N##y = B##y, max##N##y = A##y; break; \
      case 2:  min##N##x = B##x; max##N##x = A##x; min##N##y = A##y, max##N##y = B##y; break; \
      case 3:  min##N##x = A##x; max##N##x = B##x; min##N##y = A##y, max##N##y = B##y; break; \
      }

#define COMPUTE_STATE(state, S, N)            \
    state = 0; \
    if(S##x >= min##N##x) state+=4; \
    if(S##x >  max##N##x) state+=4; \
    if(S##y >= min##N##y) state+=12; \
    if(S##y >  max##N##y) state+=12; \

    /* 0 1    0  4  8
       2 3   12 16 20
             24 28 32 */
#define UPDATE_STATE(state, quadrant, skip, S, N)       \
    switch(state + quadrant) { \
    case 1:  if(S##x >= min##N##x) { skip##c1: if(S##x > max##N##x) state = 8; else state = 4; } /*f*/ \
    case 0:  goto skip; \
    case 3:  if(S##x >= min##N##x) { if(S##y >= min##N##y) break; goto skip##c1; } /*f*/ \
    case 2:  if(S##y >= min##N##y) { if(S##y > max##N##y) state = 24; else state = 12; } goto skip; \
    \
    case 6:  if(S##y >= min##N##y) break; /*f*/ \
    case 4:  if(S##x < min##N##x) state = 0; goto skip; \
    case 7:  if(S##y >= min##N##y) break; /*f*/ \
    case 5:  if(S##x > max##N##x) state = 8; goto skip; \
    \
    case 8:  if(S##x <= max##N##x) { skip##c8: if(S##x < min##N##x) state = 0; else state = 4; } /*f*/ \
    case 9:  goto skip; \
    case 10: if(S##x <= max##N##x) { if(S##y >= min##N##y) break; goto skip##c8; } /*f*/ \
    case 11: if(S##y >= min##N##y) { if(S##y > max##N##y) state = 32; else state = 20; } goto skip; \
    \
    case 13: if(S##x >= min##N##x) break; /*f*/ \
    case 12: if(S##y < min##N##y) state = 0;  goto skip; \
    case 15: if(S##x >= min##N##x) break; /*f*/ \
    case 14: if(S##y > max##N##y) state = 24; goto skip; \
      /* 16-19 fall through */ \
    case 20: if(S##x <= max##N##x) break; /*f*/ \
    case 21: if(S##y < min##N##y) state = 8;  goto skip; \
    case 22: if(S##x <= max##N##x) break; /*f*/ \
    case 23: if(S##y > max##N##y) state = 32; goto skip; \
    \
    case 25: if(S##x >= min##N##x) { if(S##y <= max##N##y) break; goto skip##c27; } /*f*/ \
    case 24: if(S##y <= max##N##y) { if(S##y < min##N##y) state = 0; else state = 12; } goto skip; \
    case 27: if(S##x >= min##N##x) { skip##c27: if(S##x > max##N##x) state = 32; else state = 28; } /*f*/ \
    case 26: goto skip; \
    \
    case 28: if(S##y <= max##N##y) break; /*f*/ \
    case 30: if(S##x < min##N##x) state = 24; goto skip; \
    case 29: if(S##y <= max##N##y) break; /*f*/ \
    case 31: if(S##x > max##N##x) state = 32; goto skip; \
    \
    case 32: if(S##x <= max##N##x) { if(S##y <= max##N##y) break; goto skip##c34; } /*f*/ \
    case 33: if(S##y <= max##N##y) { if(S##y < min##N##y) state = 8; else state = 20; } goto skip; \
    case 34: if(S##x <= max##N##x) { skip##c34: if(S##x < min##N##x) state = 24; else state = 28; } /*f*/ \
    case 35: goto skip; \
    }

/* This function is the heart of the route map algorithm.
   Essentially search for intersecting line segments, and flip them correctly
   while maintaining a skip list.
 */
bool Normalize(IsoRouteList &rl, IsoRoute *route1, IsoRoute *route2, int level, bool inverted_regions)
{
  bool normalizing;

reset:
  SkipPosition *spend=route1->skippoints, *ssend=route2->skippoints;

  if(!spend || spend->prev == spend->next) { /* less than 3 items */
      delete route1;
      if(route1 != route2)
          rl.push_back(route2);
    return true;
  }

  if(route1 == route2) {
    normalizing = true;
  } else {
    if(!ssend || ssend->prev == ssend->next) { /* less than 3 items */
      delete route2;
      if(spend)
        rl.push_back(route1);
      return true;
    }

    normalizing = false;
  }

  SkipPosition *sp = spend;
startnormalizing:
  do {

    SkipPosition *sq = sp->next;
    SkipPosition *sr, *ss;
    if(normalizing)
        ss = sp;
    else
        ss = ssend;

    Position *p = sp->point, *q = sq->point;
    double px = p->lon, qx = q->lon, py = p->lat, qy = q->lat;
        
    double minx, maxx, miny, maxy;
    COMPUTE_MIN_MAX(sp->quadrant, p, q,)
        
    Position *r, *s = ss->point;

    int dir;
    double rx, ry;
    double sx = s->lon, sy = s->lat;

    int state, rstate, pstate;
    COMPUTE_STATE(state, s,)

    int nr;
    Position *pstart, *pend, *rstart, *rend;

    do {
    sr = ss;
    ss = sr->next;

    s = ss->point;
    sx = s->lon, sy = s->lat;

    UPDATE_STATE(state, sr->quadrant, skip, s,)

    nr = 0;
    if(normalizing) {
      if(sp == sr) {
        nr = 1; /* only occurs during normalizing (first round) */
        /* normalizing and overlapping round.. don't bother to calculate smaller bounds */
        pstart = sp->point;
        pend = sq->point;
      
        rstart = sr->point;
        rend = ss->point;
        goto skip_bounds_compute;
      }
      else if (sq == sr)
        nr = 2; /* only occurs normalizing (second round) */
      else if(ss == sp)
        nr = 3; /* only occurs normalizing (last round) */
    }

#if 1 /* this is only slightly faster, barely can measure a difference */
    /* compute bounds for these skip segments */
    double minrx, maxrx, minry, maxry;
    rx = sr->point->lon, ry = sr->point->lat;
    COMPUTE_MIN_MAX(sr->quadrant, r, s, r)
        
    pstart = pend = NULL;
    q = sp->point;
    qx = q->lon, qy = q->lat;
    COMPUTE_STATE(pstate, q, r)
    do {
      p = q;
      q = q->next;
      qx = q->lon, qy = q->lat;
      UPDATE_STATE(pstate, sp->quadrant, skipp, q, r)
      if(!pstart)
        pstart = p;
      pend = q;
      COMPUTE_STATE(pstate, q, r)
      goto startingp;
      skipp:
      if(pstart)
        break; /* have start, must be done */
    startingp:;
    } while(q != sq->point);
    p = pstart;
    if(!pstart)
      goto done;
//    if(pstart == pend)  // this is never hit in practice
//      goto done;
    
    rstart = rend = NULL;
    s = sr->point;
    rstate = state; /* still valid from before */
    do {
      r = s;
      s = s->next;
      sx = s->lon, sy = s->lat;
      UPDATE_STATE(rstate, sr->quadrant, skipr, s,)
      if(!rstart)
        rstart = r;
      rend = s;
      COMPUTE_STATE(rstate, s,)
      goto startingr;
      skipr:
      if(rstart)
        break; /* have start, must be done */
    startingr:;
    } while(s != ss->point);

    if(!rstart)
      goto done;
#else
    pstart = sp->point;
    pend = sq->point;
      
    rstart = sr->point;
    rend = ss->point;
#endif
    skip_bounds_compute:

          static int c;
          c++;
    
    p = pstart;
    do {
      q = p->next;
      
      switch(nr) {
      case 1:
        s = q;
        if(s == rend)
          goto done;
        s = s->next;
        break;
      case 2:
        s = rstart;
        if(s == q)
          s = s->next;
        break;
      case 3:
        s = rstart;
        if(rend == p)
          rend = rend->prev;
        break;
      default:
        s = rstart;
      }

      if(s == rend)
        goto done;

      px = p->lon, py = p->lat;
      qx = q->lon, qy = q->lat;

      double minpqx, maxpqx, minpqy, maxpqy;
      COMPUTE_MIN_MAX(sp->quadrant, p, q, pq)

      sx = s->lon, sy = s->lat;
      COMPUTE_STATE(state, s, pq);
      do {
        r = s;
        s = r->next;

        sx = s->lon, sy = s->lat;
        UPDATE_STATE(state, sr->quadrant, skippr, s, pq);

        rx = r->lon, ry = r->lat;
        dir = TestIntersectionXY(px, py, qx, qy, rx, ry, sx, sy);
        switch(dir) {
        case -2:
          route1->skippoints = spend;
          route2->skippoints = ssend;
#define RESOLVE_REMOVAL 1  // switch to try pertubation
#if RESOLVE_REMOVAL
          route1->RemovePosition(sp, p);
#else 
            printf("-2\n");
         route1->PerturbPosition(sp, p);
#endif
          goto reset;
        case -3:
          route1->skippoints = spend;
          route2->skippoints = ssend;
#if RESOLVE_REMOVAL
          route1->RemovePosition(sq, q);
#else
            printf("-3\n");
          route1->PerturbPosition(sp, q);
#endif
          goto reset;
        case 2:
          route1->skippoints = spend;
          route2->skippoints = ssend;
#if RESOLVE_REMOVAL
          route2->RemovePosition(sr, r);
#else
            printf("2\n");
          route2->PerturbPosition(sr, r);
#endif
          goto reset;
        case 3:
          route1->skippoints = spend;
          route2->skippoints = ssend;
#if RESOLVE_REMOVAL
          route2->RemovePosition(ss, s);
#else
            printf("3\n");
          route2->PerturbPosition(sr, s);
#endif
          goto reset;
        case -1:
        case 1:
          if(!normalizing) { /* sanity check for merging */
            if(dir == -1) {
              if(route1->direction != 1 || route2->direction != -1)
                /* we intersected at the wrong side, skip this intersection
                   and continue to find the intersection we want,  this occurs
                   when a line segment passes completely through a region.
                   We could possibly merge here anyway but the result
                   would be less correct.  */
                goto skipmerge;
            } else
              /* inverted invalid test */
              if(route1->direction == 1 && route2->direction == -1)
                goto skipmerge;
          }

          SwapSegments(p, q, r, s); /* update position list */
          SwapSkipSegments(sp, sq, sr, ss); /* update skip lists */

          /* now update skip list properly */
          if(sp->quadrant != sr->quadrant) {
            int rquadrant = sr->quadrant, pquadrant = sp->quadrant;
            FixSkipList(sp, ss, p, s, rquadrant, spend, ssend);
            FixSkipList(sr, sq, r, q, pquadrant, spend, ssend);
          }
          
          if(normalizing) {
            /* did the end end up in the subroute? move it back out */
            if(UpdateEnd(spend, sr))
              spend = sp->next;
            if(UpdateEnd(ssend, sr))
              ssend = sp->next;

            if(level == 0) {
              if(dir != route1->direction || sr->next->next == sr) { /* slight numerical error, or outer inversion */
                DeletePoints(r);
                DeleteSkipPoints(sr);
              } else {
                IsoRoute *x = new IsoRoute(sr, dir);
                IsoRouteList sub;
                Normalize(sub, x, x, level + 1, inverted_regions);
                if(inverted_regions) {
                  for(IsoRouteList::iterator it = sub.begin(); it != sub.end(); ++it) {
                    if((*it)->children.size()) {
                      printf("grandchild detected\n");
                      delete *it;
                    } else if(route1->direction == (*it)->direction) {
                      rl.push_back(*it); /* sibling */
                    } else if((*it)->Count() < 16) {
                      printf("too small to be a useful child: %d\n", (*it)->Count());
                      delete *it;
                    } else if(!(route1->skippoints=spend, route1->CompletelyContained(*it))) {
                      printf("not correct to be child: %d\n", (*it)->Count());
                      delete *it;
                    } else { /* different direction contained.. it is a child */
                      /* we should merge it with the other children here */
                      printf("Child route: %d\n", (*it)->Count());
                      IsoRoute *child = *it;
                      child->parent = route1;
                      route1->children.push_back(child);
                    }
                  }
                } else { /* no inverted regions mode */
                  for(IsoRouteList::iterator it = sub.begin(); it != sub.end(); ++it) {
                    if(route1->direction == (*it)->direction) {
                      rl.push_back(*it); /* sibling */
                    } else
                      delete *it; /* inversion */
                  }
                }
              }
            } else { /* all subregions are siblings for inner levels */

              if(sr->next->next == sr) { /* slight numerical error, or outer inversion */
                DeletePoints(r);
                DeleteSkipPoints(sr);
              } else {

              IsoRoute *x = new IsoRoute(sr, dir);
              IsoRouteList sub;
              Normalize(sub, x, x, level + 1, inverted_regions);
              rl.splice(rl.end(), sub);
              }
            }
          } else { /* merging */
            for(IsoRouteList::iterator it = route2->children.begin();
                it != route2->children.end(); it++)
                (*it)->parent = route1;
                
            /* merge children (append is currently incorrect)
               the children need to be merged, and any overlapping regions
               incremented so they don't get removed if contained */
            int sc1 = route1->children.size();
            int sc2 = route2->children.size();
            if(sc1 && sc2)
                printf("both have children: %d %d\n", sc1, sc2);
            
            route1->children.splice(route1->children.end(), route2->children);
            route2->skippoints = NULL; /* all points are now in route1 */
            delete route2;
            route2 = route1;
            ssend = spend;
            spend = sr->next; /* after old sq we are done.. this is known */
            /* continue from here and begin to normalize */
#if 0 /* these only needed if we could jump back in too a more optimal spot than startnormalizing */
            /*  could in theory somehow skip to p for this round instead of starting
                at sp->point.. but I doubt it would speed things up that much. */
            sr = sp, ss = sr->next;
            pend = rend = ss->point;
#endif
            normalizing = true;
          }
          goto startnormalizing;
        }
      skipmerge:        
        COMPUTE_STATE(state, s, pq);
      skippr:;
      } while(s != rend);
      p = q;
    } while(p != pend);
 done:
    COMPUTE_STATE(state, s,)
 skip:;
    } while(ss != ssend);
  sp = sq;
} while(sp != spend);

  if(normalizing) {
    route1->skippoints = spend;
    rl.push_back(route1);
    return true;
  }
  return false;
}

/* take two routes that may overlap, and combine into a list of non-overlapping routes */
bool Merge(IsoRouteList &rl, IsoRoute *route1, IsoRoute *route2, int level, bool inverted_regions)
{
    if(route1->direction == -1 && route2->direction == -1) {
        printf("cannot merge two inverted routes\n");
        exit(1);
    }

    /* quick test to make sure we could possibly intersect with bounds */
    double bounds1[4], bounds2[4];
    route1->FindIsoRouteBounds(bounds1);
    route2->FindIsoRouteBounds(bounds2);
    if(bounds1[MINLAT] > bounds2[MAXLAT] || bounds1[MAXLAT] < bounds2[MINLAT] ||
       bounds1[MINLON] > bounds2[MAXLON] || bounds1[MAXLON] < bounds2[MINLON])
        return false;

    /* make sure route1 is on the outside */
    if(route2->skippoints->point->lat > route1->skippoints->point->lat) {
        IsoRoute *t = route1;
        route1 = route2;
        route2 = t;
    }

    if(Normalize(rl, route1, route2, level, inverted_regions))
        return true;

    /* no intersection found, test if the second route is completely
       inside the first */
    if(route1->ContainsRoute(route2)) {
        if(inverted_regions) {
            if(route1->direction == 1 && route2->direction == 1) {
                int sc1 = route1->children.size();
                int sc2 = route2->children.size();
                    /* if both region have children, they should get merged
                       correctly here instead of this */
                if(sc1 && sc2)
                    printf("both have children contains: %d %d\n", sc1, sc2);

                /* remove all of route2's children, route1 clears them
                   (unless they interected with route1 children which we don't handle yet */
                for(IsoRouteList::iterator it2 = route2->children.begin();
                    it2 != route2->children.end(); it2++)
                    delete *it2;
                route2->children.clear();
                
                /* now determine if route2 affects any of route1's children,
                   if there are any intersections, it should mask away that area.
                   once completely merged, all the masks are removed and children
                   remain */
                IsoRouteList childrenmask; /* non-inverted */
                IsoRouteList mergedchildren; /* inverted */
                childrenmask.push_back(route2);
                while(childrenmask.size() > 0) {
                    IsoRoute *r1 = childrenmask.front();
                    childrenmask.pop_front();
                    while(route1->children.size() > 0) {
                        IsoRoute *r2 = route1->children.front();
                        route1->children.pop_front();
                        IsoRouteList childrl; /* see if there is a merge */

                        if(Merge(childrl, r1, r2, 1, true)) { 
                            for(IsoRouteList::iterator cit = childrl.begin(); cit != childrl.end(); cit++)
                                if((*cit)->direction == route1->direction)
                                    childrenmask.push_back(*cit);
                                else {
                                    IsoRoute *child = *cit;
                                    child->parent = route1;
                                    route1->children.push_back(child);
                                }
                            goto remerge_children;
                        } else
                            mergedchildren.push_back(r2);
                    }
                    delete r1; /* all children have been tried, so done with this mask */
                    
                remerge_children:
                    route1->children.splice(route1->children.end(), mergedchildren);
                }
            } else if(route1->direction == -1 && route2->direction == -1) {
                delete route1; /* keep smaller region if both inverted */
                route1 = route2;
            } else if(route1->direction == 1 && route2->direction == -1) {
                delete route2;
            } else {
                /* this is a child route with a normal route completely inside..
                   a contrived situation it is, should not get here often */
                printf("contrived delete: %d, %d\n", route1->Count(), route2->Count());
                delete route2;
            }
        } else /* no inverted regions mode */
            delete route2; /* it covers a sub area, delete it */

        rl.push_back(route1); /* no need to normalize */
        return true;
    }

    /* routes close enough to pass initial rectangle test but no
       actual intersection or overlap occurs so no merge takes places */
    return false;
}

/* much faster than "a->Distance(b);" */
static double SimpleDistance(Position *a, Position *b)
{
    double dlat = a->lat - b->lat, dlon = fabs(a->lon - b->lon);
    while(dlon > 180) dlon -= 360;
    return dlat*dlat + dlon*dlon;
}

/* find closest position in the routemap */
Position *IsoRoute::ClosestPosition(double lat, double lon, double *dist)
{
    double mindist = INFINITY;

    /* first find closest skip position */
    SkipPosition *s = skippoints;
    Position pos(lat, lon), *minpos = s->point;
    int lq = -1;
    do {
        int q1 = ComputeQuadrant(&pos, s->point);

        bool dotest = true;
        if(q1 == s->quadrant)
            dotest = false;
        /* tangent, maybe we can avoid testing in rabbit cases */
        else if(lq == s->quadrant && q1 != (lq^3) && q1 == s->prev->quadrant)
            dotest = false;
        lq = q1;

        if(dotest) {
            Position *p = s->point, *e = s->next->point;
            do {
                double dist = SimpleDistance(&pos, p);
                if(dist < mindist) {
                    minpos = p;
                    mindist = dist;
                }
                p = p->next;
            } while(p != e);
        }

        s = s->next;
    } while(s != skippoints);

    mindist = pos.Distance(minpos);

    /* now try children */
    for(IsoRouteList::iterator it = children.begin(); it != children.end();  it++) {
        double dist;
        Position *p = (*it)->ClosestPosition(lat, lon, &dist);
        if(/*p &&*/ dist < mindist) {
            minpos = p;
            mindist = dist;
        }
    }
    if(dist)
        *dist = mindist;
    return minpos;
}

bool IsoRoute::Propagate(IsoRouteList &routelist, GribRecordSet *grib,
                         wxDateTime &time, RouteMapConfiguration &configuration)
{
    Position *p = skippoints->point;
    bool ret = false;
    if(p)
        do {
            if(p->Propagate(routelist, grib, time, configuration))
                ret = true;
            p = p->next;
        } while(p != skippoints->point);
    return ret;
}

int IsoRoute::SkipCount()
{
    SkipPosition *s = skippoints;
    int count = 0;
    if(s)
        do {
            count++;
            s = s->next;
        } while(s != skippoints);
    return count;
}

int IsoRoute::Count()
{
    Position *p = skippoints->point;
    int count = 0;
    if(p)
        do {
            count++;
            p = p->next;
        } while(p != skippoints->point);
    return count;
}

void IsoRoute::UpdateStatistics(int &routes, int &invroutes, int &skippositions, int &positions)
{
    invroutes += children.size();
    routes += children.size() + 1;

    for(IsoRouteList::iterator it = children.begin(); it != children.end(); it++)
        skippositions += (*it)->SkipCount();
    skippositions += SkipCount();

    for(IsoRouteList::iterator it = children.begin(); it != children.end(); it++)
        positions += (*it)->Count();
    positions += Count();
}

IsoChron::IsoChron(IsoRouteList r, wxDateTime t, GribRecordSet *g)
    : routes(r), time(t), m_Grib(g)
{
}

IsoChron::~IsoChron()
{
    for(IsoRouteList::iterator it = routes.begin(); it != routes.end(); ++it)
        delete *it;

    /* done with grib */
    if(m_Grib) {
        for(int i=0; i<Idx_COUNT; i++)
            delete m_Grib->m_GribRecordPtrArray[i];
        delete m_Grib;
    }
}

void IsoChron::PropagateIntoList(IsoRouteList &routelist, GribRecordSet *grib,
                                 wxDateTime &time, RouteMapConfiguration &configuration)
{
    for(IsoRouteList::iterator it = routes.begin(); it != routes.end(); ++it) {
        bool propagated = false;
        
        IsoRoute *x;
        /* if anchoring is allowed, then we can propagate a second time,
           so copy the list before clearing the propagate flag,
           when depth data is implemented we will need to flag positions as propagated
           if they are too deep to anchor here. */
        if(configuration.Anchoring)
            x = new IsoRoute(*it);

        /* build up a list of iso regions for each point
           in the current iso */
        if((*it)->Propagate(routelist, grib, time, configuration))
            propagated = true;
        
        if(!configuration.Anchoring)
            x = new IsoRoute(*it);

        for(IsoRouteList::iterator cit = (*it)->children.begin();
            cit != (*it)->children.end(); cit++) {
            IsoRoute *y;
            if(configuration.Anchoring)
                y = new IsoRoute(*cit, x);
            else
                y = NULL;
            if((*cit)->Propagate(routelist, grib, time, configuration)) {
                if(!configuration.Anchoring)
                    y = new IsoRoute(*cit, x);
                x->children.push_back(y); /* copy child */
                propagated = true;
            } else
                delete y;
        }

        /* if any propagation occured even for children, then we clone this route
           this prevents backtracking, otherwise, we don't need this route
           (it's a dead end) */
        if(propagated) {
#if 0
                y = new IsoRoute(x);
                for(IsoRouteList::iterator cit = x->children.begin();
                    cit != x->children.end(); cit++)
                    y->children.push_back(new IsoRoute(*cit, y)); /* copy child */
                if(x->ApplyCurrents(grib, time, configuration))
                    routelist.push_back(y);
                else
                    delete y; /* I guess we didn't need it after all */
            } else
                x->ApplyCurrents(grib, time, configuration);
#endif
            routelist.push_back(x);
        } else
            delete x; /* didn't need it */
    }
}

bool IsoChron::Contains(Position &p)
{
    for(IsoRouteList::iterator it = routes.begin(); it != routes.end(); ++it)
        switch((*it)->Contains(p, true)) {
        case -1: printf("-1 contains!\n");
        case 0: continue;
        default: return true;
        }
    return false;
}

bool IsoChron::Contains(double lat, double lon)
{
    Position p(lat, lon);
    return Contains(p);
}

Position* IsoChron::ClosestPosition(double lat, double lon, double *dist)
{
    Position *minpos = NULL;
    double mindist = INFINITY;
    for(IsoRouteList::iterator it = routes.begin(); it != routes.end(); ++it) {
        double dist;
        Position *pos = (*it)->ClosestPosition(lat, lon, &dist);
        if(pos && dist < mindist) {
            minpos = pos;
            mindist = dist;
        }
    }
    if(dist)
        *dist = mindist;
    return minpos;
}

bool RouteMapConfiguration::Update()
{
    bool havestart = false, haveend = false;
    for(std::list<RouteMapPosition>::iterator it = RouteMap::Positions.begin();
        it != RouteMap::Positions.end(); it++) {
        if(Start == (*it).Name) {
            StartLat = (*it).lat;
            StartLon = (*it).lon;
            havestart = true;
        }
        if(End == (*it).Name) {
            EndLat = (*it).lat;
            EndLon = (*it).lon;
            haveend = true;
        }
    }

    if(!havestart || !haveend)
        return false;

    if((positive_longitudes = fabs(average_longitude(StartLon, EndLon)) > 90)) {
        StartLon = positive_degrees(StartLon);
        EndLon = positive_degrees(EndLon);
    }

    ll_gc_ll_reverse(StartLat, StartLon, EndLat, EndLon, &StartEndBearing, 0);
    return true;
}

bool (*RouteMap::ClimatologyData)
(int setting, const wxDateTime &, double, double, double &, double &) = NULL;
bool (*RouteMap::ClimatologyWindAtlasData)(const wxDateTime &, double, double, int &count,
                                           double *, double *, double &, double &) = NULL;
int (*RouteMap::ClimatologyCycloneTrackCrossings)(double, double, double, double,
                                                  const wxDateTime &, int, int,
                                                  const wxDateTime &) = NULL;

std::list<RouteMapPosition> RouteMap::Positions;

RouteMap::RouteMap()
{
}

RouteMap::~RouteMap()
{
    Clear();
}

void RouteMap::PositionLatLon(wxString Name, double &lat, double &lon)
{
    for(std::list<RouteMapPosition>::iterator it = Positions.begin();
        it != Positions.end(); it++)
        if((*it).Name == Name) {
            lat = (*it).lat;
            lon = (*it).lon;
        }
}

bool RouteMap::ReduceList(IsoRouteList &merged, IsoRouteList &routelist, RouteMapConfiguration &configuration)
{
    IsoRouteList unmerged;
    while(routelist.size()) {
        if(TestAbort())
            return false;

        IsoRoute *r1 = routelist.front();
        routelist.pop_front();
        while(routelist.size()) {
            IsoRoute *r2 = routelist.front();
            routelist.pop_front();
            IsoRouteList rl;

//            extern int debugsize, debugcnt, debuglimit;
            if(
#if 0
              (origin.size() < debugsize || debugcnt++ < debuglimit) &&
#endif
              Merge(rl, r1, r2, 0, configuration.InvertedRegions)) {
#if 1 /* TODO: find fastest method */
                routelist.splice(routelist.end(), rl);
#else /* merge new routes with each other right away before hitting the main list */
                IsoRouteList unmerged2;
                while(rl.size() > 0) {
                    r1 = rl.front();
                    rl.pop_front();
                    while(rl.size() > 0) {
                        r2 = rl.front();
                        rl.pop_front();
                        IsoRouteList rl2;
                        if(Merge(rl2, r1, r2, 0, configuration.InvertedRegions)) {
                            rl.splice(rl.end(), rl2);
                            goto remerge2;
                        } else
                            unmerged2.push_back(r2);
                    }
                    unmerged.push_back(r1);
                remerge2:
                    unmerged.splice(unmerged.end(), unmerged2);
                }
#endif
                goto remerge;
            } else
                unmerged.push_back(r2);
        }
        /* none more in list so nothing left to merge with */
        merged.push_back(r1);

    remerge:
        /* put any unmerged back in list to continue */
        routelist.splice(routelist.end(), unmerged);
    }
    return true;
}

/* enlarge the map by 1 level */
bool RouteMap::Propagate()
{
#if 0
    extern int debugsize;
    if((int)origin.size() > debugsize)
        return false;
#endif

    Lock();
    if(!m_bValid) {
        Unlock();
        m_bFinished = true;
        return false;
    }

    bool notready = m_bFinished || m_bNeedsGrib;

    if(notready) {
        Unlock();
        return false;
    }

    if(m_Configuration.UseGrib &&
       (!m_NewGrib ||
        !m_NewGrib->m_GribRecordPtrArray[Idx_WIND_VX] ||
        !m_NewGrib->m_GribRecordPtrArray[Idx_WIND_VY]) &&
       m_Configuration.ClimatologyType == RouteMapConfiguration::DISABLED &&
       !m_Configuration.AllowDataDeficient) {
        m_bFinished = true;
        m_bGribFailed = true;
        Unlock();
        return false;
    }

    if((!m_NewGrib ||
        !m_NewGrib->m_GribRecordPtrArray[Idx_WIND_VX] ||
        !m_NewGrib->m_GribRecordPtrArray[Idx_WIND_VY]) &&
       m_Configuration.ClimatologyType != RouteMapConfiguration::DISABLED
       && ClimatologyData &&
       !m_Configuration.AllowDataDeficient) {
        double WG, VWG;
        wxDateTime time = wxDateTime::Now();
        /* replace when climatology supports detecting if data is loaded
           without querying */
        if(!ClimatologyData(WIND, time, 0, 0, WG, VWG)) {
            m_bFinished = true;
            m_bClimatologyFailed = true;
            Unlock();
            return false;
        }
    }

    /* test for cyclone data if needed */
    if(m_Configuration.AvoidCycloneTracks &&
       (!ClimatologyCycloneTrackCrossings ||
        ClimatologyCycloneTrackCrossings(0, 0, 0, 0, wxDateTime(), 0, 0, wxDateTime())==-1)) {
        m_bFinished = true;
        m_bClimatologyFailed = true;
        Unlock();
        return false;
    }


    if(!m_Configuration.UseGrib && m_Configuration.ClimatologyType == RouteMapConfiguration::DISABLED) {
        m_bFinished = true;
        m_bNoData = true;
        Unlock();
        return false;
    }

    GribRecordSet *grib = m_NewGrib;

    wxDateTime time = m_NewTime;
    RouteMapConfiguration configuration = m_Configuration;
    IsoRouteList routelist;
    Unlock();

    if(origin.size())
        origin.back()->PropagateIntoList(routelist, grib, time, configuration);
    else {
        Position *np = new Position(configuration.StartLat, configuration.StartLon);
        np->prev = np->next = np;
        routelist.push_back(new IsoRoute(np->BuildSkipList()));
    }

    Lock();
    m_NewGrib = NULL;
    m_NewTime = time + wxTimeSpan(0, 0, configuration.dt);
    m_bNeedsGrib = configuration.UseGrib;
    Unlock();

    IsoChron* update;
    if(routelist.size()) {
        IsoRouteList merged;
        if(!ReduceList(merged, routelist, configuration)) {
            Unlock();
            return false;
        }
        update = new IsoChron(merged, time, grib);
    } else
        update = NULL;

    Lock();
    if(update) {
        origin.push_back(update);
        if(update->Contains(m_Configuration.EndLat, m_Configuration.EndLon)) {
            m_bFinished = true;
            m_bReachedDestination = true;
        }
    } else
        m_bFinished = true;

    Unlock();
    return true;
}

Position *RouteMap::ClosestPosition(double lat, double lon, double *dist, bool before_last)
{
    if(!origin.size())
        return NULL;

    Position *minpos = NULL;
    double mindist = INFINITY;
    Lock();

    IsoChronList::iterator last = origin.end();
    if(before_last)
        last--;

    for(IsoChronList::iterator it = origin.begin(); it != last; ++it) {
        double dist;
        Position *pos = (*it)->ClosestPosition(lat, lon, &dist);
        
        if(pos && dist < mindist) {
            minpos = pos;
            mindist = dist;
        }
    }
    Unlock();

    if(dist)
        *dist = mindist;
    return minpos;
}

wxString RouteMap::Reset()
{
    Lock();
    Clear();

    wxString boaterror = m_Configuration.boat.OpenXML(m_Configuration.boatFileName);

    m_NewGrib = NULL;
    m_NewTime = m_Configuration.StartTime;
    m_bNeedsGrib = m_Configuration.UseGrib;

    m_bReachedDestination = false;
    m_bGribFailed = false;
    m_bClimatologyFailed = false;
    m_bNoData = false;
    m_bFinished = false;

    Unlock();

    return boaterror;
}

void RouteMap::SetNewGrib(GribRecordSet *grib)
{
    Lock();
    m_bNeedsGrib = !grib;

    /* copy the grib record set */
    if(grib) {
        m_NewGrib = new GribRecordSet;
        m_NewGrib->m_Reference_Time = grib->m_Reference_Time;
        for(int i=0; i<Idx_COUNT; i++) {
            if(grib->m_GribRecordPtrArray[i])
                m_NewGrib->m_GribRecordPtrArray[i] = new GribRecord
                    (*grib->m_GribRecordPtrArray[i]);
            else
                m_NewGrib->m_GribRecordPtrArray[i] = NULL;
        }
    }
    Unlock();
}

void RouteMap::GetStatistics(int &isochrons, int &routes, int &invroutes, int &skippositions, int &positions)
{
    Lock();
    isochrons = origin.size();
    routes = invroutes = skippositions = positions = 0;
    for(IsoChronList::iterator it = origin.begin(); it != origin.end(); ++it)
        for(IsoRouteList::iterator rit = (*it)->routes.begin(); rit != (*it)->routes.end(); ++rit)
            (*rit)->UpdateStatistics(routes, invroutes, skippositions, positions);
    Unlock();
}

void RouteMap::Clear()
{
    for(IsoChronList::iterator it = origin.begin(); it != origin.end(); ++it)
        delete *it;

    origin.clear();
}
