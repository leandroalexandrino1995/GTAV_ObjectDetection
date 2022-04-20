#define NOMINMAX

#include "LiDAR.h"
#include "ObjectDetection.h"
#include <math.h>
#include <stdio.h>
#include <cmath>
#include <string>
#include <sstream>
#include <algorithm>
#include <fstream>

#include <boost/random.hpp>
#include <boost/math/distributions/normal.hpp>

#include "Functions.h"
#include "Constants.h"

boost::random::mt19937 s_rng;
boost::random::normal_distribution<> s_nDist(DEPTH_NOISE_MEAN, DEPTH_NOISE_STDDEV);

LiDAR::LiDAR()
{
    m_pPointClouds = NULL;
    m_pRaycastPointCloud = NULL;
    m_lidar2DPoints = NULL;
    m_updatedPointCloud = NULL;
    m_pointsHit = 0;
    m_maxRange = 0;
    m_vertiUpLimit = 0;
    m_vertiUnLimit = 0;
    m_horizLeLimit = 0;
    m_horizRiLimit = 0;
    m_vertiSmplNum = 0;
    m_horizSmplNum = 0;
    m_vertiResolu = 0;
    m_horizResolu = 0;
    m_camera = 0;
    m_lidarVehicle = 0;
    m_initType = _LIDAR_NOT_INIT_YET_;
    m_isAttach = false;
    VehicleLookUpTable();
}

LiDAR::~LiDAR()
{
    DestroyLiDAR();
}

void LiDAR::Init2DLiDAR_SmplNum(float maxRange, int horizSmplNum, float horizLeLimit, float horizRiLimit)
{
    if (m_initType != _LIDAR_NOT_INIT_YET_)
        DestroyLiDAR();

    m_horizSmplNum = horizSmplNum;
    m_maxRange = maxRange;
    if (horizRiLimit <= horizLeLimit)
    {
        printf("\nHorizontal FOV angle parameters error");
        return;
    }
    m_horizLeLimit = horizLeLimit;
    m_horizRiLimit = horizRiLimit;
    m_horizResolu = (m_horizLeLimit + 360.0 - m_horizRiLimit) / m_horizSmplNum;
    if (!m_pPointClouds)
        m_pPointClouds = (float *)malloc(m_horizSmplNum * sizeof(float));
    if (m_pPointClouds == NULL)
        printf("\nLiDAR: memory alloc err");

    m_initType = _LIDAR_INIT_AS_2D_;

#ifdef DEBUG_CONFIG
    printf("\nDEBUG_CONFIG: function: %s", __FUNCTION__);
    printf("\ncamera=%d, ownCar=%d, maxRange=%f, horizSmplNum=%d, horizLeLimit=%f, horizRiLimit=%f",
        m_camera, m_lidarVehicle, m_maxRange, m_horizSmplNum, m_horizLeLimit, m_horizRiLimit);
    printf("\nHorizontal FOV(yaw definition): %f to %f", -m_horizLeLimit, 360.0 - m_horizRiLimit);
    printf("\nHorizontal angel resolution(deg): %f", m_horizResolu);
    printf("\n");
#endif // DEBUG_CONFIG
}

void LiDAR::Init3DLiDAR_SmplNum(float maxRange, int horizSmplNum, float horizLeLimit, float horizRiLimit,
    int vertiSmplNum, float vertiUpLimit, float vertiUnLimit)
{
    std::ostringstream oss;
    oss << "Vertical sample number: " << vertiSmplNum;
    std::string str = oss.str();
    log(str);

    if (m_initType != _LIDAR_NOT_INIT_YET_)
        DestroyLiDAR();

    m_vertiSmplNum = vertiSmplNum;
    m_horizSmplNum = horizSmplNum;
    m_maxRange = maxRange;
    //Vertical:
    if (vertiUnLimit <= vertiUpLimit)
    {
        printf("\nVertical FOV angle parameters error");
        return;
    }
    m_vertiUpLimit = vertiUpLimit;
    m_vertiUnLimit = vertiUnLimit;
    m_vertiResolu = (m_vertiUnLimit - m_vertiUpLimit) / m_vertiSmplNum;

    //Horizontal: 
    if (horizRiLimit <= horizLeLimit)
    {
        printf("\nHorizontal FOV angle parameters error");
        return;
    }
    m_horizLeLimit = horizLeLimit;
    m_horizRiLimit = horizRiLimit;
    m_horizResolu = (m_horizLeLimit + 360.0 - m_horizRiLimit) / m_horizSmplNum;

    if (!m_pPointClouds)
        //Malloc 4*max number of points bytes for the point cloud
        m_pPointClouds = (float*)malloc((MAX_POINTS * sizeof(float)));
    if (m_pPointClouds == NULL)
        printf("\nLiDAR: memory alloc err");

    if (!m_pRaycastPointCloud)
        //Malloc 4*max number of points bytes for the point cloud
        m_pRaycastPointCloud = (float*)malloc((MAX_POINTS * sizeof(float)));
    if (m_pRaycastPointCloud == NULL)
        printf("\nLiDAR: memory alloc err3");

    if (!m_updatedPointCloud)
        //Malloc 4*max number of points bytes for the point cloud
        m_updatedPointCloud = (float*)malloc((MAX_POINTS * sizeof(float)));
    if (m_updatedPointCloud == NULL)
        printf("\nLiDAR: memory alloc err4");

    //Malloc 2*max number of points bytes for the 3d -> 2d point map
    if (GENERATE_2D_POINTMAP) {
        m_lidar2DPoints = (float*)malloc((MAX_POINTS * sizeof(float)) / 2);
        if (m_lidar2DPoints == NULL)
            printf("\nLiDAR: memory alloc err2");
    }

    m_initType = _LIDAR_INIT_AS_3D_;


#ifdef DEBUG_CONFIG
    printf("\nDEBUG_CONFIG: function: %s", __FUNCTION__);
    printf("\ncamera=%d, ownCar=%d, maxRange=%f, horizSmplNum=%d, horizLeLimit=%f, horizRiLimit=%f, vertiSmplNum=%d, vertiUpLimit=%f, vertiUnLimit=%f",
        m_camera, m_lidarVehicle, m_maxRange, m_horizSmplNum, m_horizLeLimit, m_horizRiLimit, m_vertiSmplNum, m_vertiUpLimit, m_vertiUnLimit);
    printf("\nHorizontal FOV(yaw definition): %f to %f", -m_horizLeLimit, 360.0 - m_horizRiLimit);
    printf("\nVertical FOV(pitch definition): %f to %f", 90.0 - m_vertiUpLimit, 90.0 - m_vertiUnLimit);
    printf("\nHorizontal angel resolution(deg): %f", m_horizResolu);
    printf("\nVertical angel resolution(deg): %f", m_vertiResolu);
    printf("\n");
#endif // DEBUG_CONFIG
}

void LiDAR::Init2DLiDAR_FOV(float maxRange, float horizFOV, float horizAngResolu)
{
    Init2DLiDAR_SmplNum(maxRange, horizFOV / horizAngResolu, horizFOV / 2, 360.0 - horizFOV / 2);
}

void LiDAR::Init3DLiDAR_FOV(float maxRange, float horizFOV, float horizAngResolu, float vertiFOV, float vertiAngResolu, float vertiUpLimit)
{
    Init3DLiDAR_SmplNum(maxRange, horizFOV / horizAngResolu, horizFOV / 2, 360.0 - horizFOV / 2, vertiFOV / vertiAngResolu, 90.0 - vertiUpLimit, 90.0 + vertiFOV - vertiUpLimit);
}

void LiDAR::AttachLiDAR2Camera(Cam camera, Entity ownCar)
{
    if (!m_isAttach)
    {
        m_camera = camera;
        m_lidarVehicle = ownCar;
        m_isAttach = true;
        log("LiDAR attached to car");
    }
    else
    {
        log("LiDAR is already attached to car");
    }
}

void LiDAR::DestroyLiDAR()
{
    if (m_pPointClouds) {
        free(m_pPointClouds);
        m_pPointClouds = NULL;
    }
    if (m_pRaycastPointCloud) {
        free(m_pRaycastPointCloud);
        m_pRaycastPointCloud = NULL;
    }
    if (m_updatedPointCloud) {
        free(m_updatedPointCloud);
        m_updatedPointCloud = NULL;
    }
    if (m_lidar2DPoints) {
        free(m_lidar2DPoints);
        m_lidar2DPoints = NULL;
    }
    m_maxRange = 0;
    m_vertiUpLimit = 0;
    m_vertiUnLimit = 0;
    m_horizLeLimit = 0;
    m_horizRiLimit = 0;
    m_vertiSmplNum = 0;
    m_horizSmplNum = 0;
    m_vertiResolu = 0;
    m_horizResolu = 0;
    m_camera = 0;
    m_lidarVehicle = 0;
    m_initType = _LIDAR_NOT_INIT_YET_;
    m_isAttach = false;
}

float * LiDAR::Get2DPoints(int &size) {
    size = m_beamCount;
    return m_lidar2DPoints;
}

float * LiDAR::GetRaycastPointcloud(int &size) {
    size = m_raycastPoints;
    return m_pRaycastPointCloud;
}

float* LiDAR::UpdatePointCloud(int &size, float* depthMap) {
    m_depthMap = depthMap;
    for (int i = 0; i < m_hitDepthPoints.size(); i++) {
        Vector3 vec_cam_coord = get3DFromDepthTarget(m_hitDepthPoints[i].target, m_hitDepthPoints[i].target2D);

        float newDistance = sqrt(SYSTEM::VDIST2(0, 0, 0, vec_cam_coord.x, vec_cam_coord.y, vec_cam_coord.z));
        if (newDistance <= MAX_LIDAR_DIST) {
            //Note: The y/x axes are changed to conform with KITTI velodyne axes
            float* p = m_updatedPointCloud + (m_updatedPointCount * FLOATS_PER_POINT);
            *p = vec_cam_coord.y;
            *(p + 1) = -vec_cam_coord.x;
            *(p + 2) = vec_cam_coord.z;
            *(p + 3) = m_hitDepthPoints[i].groundDist;//We don't have the entityID if we're using the depth map
            ++m_updatedPointCount;
        }
    }

    size = m_updatedPointCount;
    m_hitDepthPoints.clear();
    return m_updatedPointCloud;
}

void LiDAR::printDepthStats() {
    float totalRatio = 0;
    float ratio10 = 0;
    float ratio40 = 0;
    float ratio120 = 0;
    float ratioAbove100 = 0;
    int countTotal = 0;
    int count10 = 0;
    int count40 = 0;
    int count120 = 0;
    int countAbove100 = 0;
    for (int i = 0; i < m_hitDepthPoints.size(); i++) {
        Vector3 vec_cam_coord = get3DFromDepthTarget(m_hitDepthPoints[i].target, m_hitDepthPoints[i].target2D);

        float newDistance = sqrt(SYSTEM::VDIST2(0, 0, 0, vec_cam_coord.x, vec_cam_coord.y, vec_cam_coord.z));

        //Only use ground points since they aren't affected by object models
        if (m_hitDepthPoints[i].groundDist > 0) continue;

        float ratio = newDistance / m_hitDepthPoints[i].rayCastDepth;
        if (ratio < 1.1 && ratio > 0.9) {
            totalRatio += ratio;
            countTotal++;
        }
        if (newDistance > 100) {
            ratioAbove100 += ratio;
            countAbove100++;
        }

        if (newDistance <= MAX_LIDAR_DIST && ratio < 1.05 && ratio > 0.95) {
            if (newDistance <= 10) {
                ratio10 += ratio;
                count10++;
            }
            else if (newDistance <= 40) {
                ratio40 += ratio;
                count40++;
            }
            else {
                ratio120 += ratio;
                count120++;
            }
        }
    }

    std::ostringstream oss;
    oss << "Avg distance ratio: " << totalRatio/(float)countTotal <<
        "\nOver 100: " << ratioAbove100/(float)countAbove100 <<
        "\nUnder 120: " << ratio120/(float)count120 <<
        "\nUnder 40: " << ratio40/(float)count40 <<
        "\nUnder 10: " << ratio10/(float)count10;
    log(oss.str(), true);
}

float * LiDAR::GetPointClouds(int &size, std::unordered_map<int, HitLidarEntity*> *entitiesHit, int param, float* depthMap, uint32_t* pInstanceSeg, Entity perspectiveVehicle)
{
    if (perspectiveVehicle != -1) {
        m_lidarVehicle = perspectiveVehicle;
    }

    m_depthMap = depthMap;
    native_param = param;

    m_pInstanceSeg = pInstanceSeg;
    m_entitiesHit = entitiesHit;
    m_pointsHit = 0;
    m_raycastPoints = 0;
    m_depthMapPoints = 0;
    m_beamCount = 0;
    m_updatedPointCount = 0;

    if (m_pPointClouds == NULL || m_initType == _LIDAR_NOT_INIT_YET_ || !m_isAttach)
        return NULL;
    switch (m_initType)
    {
    case _LIDAR_INIT_AS_2D_: GenerateHorizPointClouds(90, m_pPointClouds);
    case _LIDAR_INIT_AS_3D_:
    {
        m_max_dist = 0;
        m_min_dist = 5555555555;

        //log("Trying to generate pointcloud");
        float phi = m_vertiUnLimit;
        int horizBeamCount = 0;
        for (int k = 0; k < m_vertiSmplNum; k++)
        {
            if (phi > m_vertiUpLimit - m_vertiResolu)
                phi = m_vertiUnLimit - k * m_vertiResolu;
            else
                break;

            ++horizBeamCount;
            GenerateHorizPointClouds(phi, m_pPointClouds);
        }
        std::ostringstream oss;
        oss << "Max distance: " << m_max_dist << " min distance: " << m_min_dist
            << "\nBeamCount: " << horizBeamCount;
        log(oss.str());
    }
    default:
        break;
    }

    std::ostringstream oss1;
    oss1 << "Raycast points: " << m_raycastPoints << " DM points: " << m_depthMapPoints << " total: " << m_pointsHit;
    std::string str1 = oss1.str();
    log(str1);

    size = m_pointsHit;
    return m_pPointClouds;
}

int LiDAR::getTotalSmplNum()
{
    switch (m_initType)
    {
    case _LIDAR_INIT_AS_2D_:
        return m_horizSmplNum;
    case _LIDAR_INIT_AS_3D_:
        return m_horizSmplNum * m_vertiSmplNum;
    default:
        return 0;
    }
}

int LiDAR::getVertiSmplNum()
{
    return m_vertiSmplNum;
}

int LiDAR::getHorizSmplNum()
{
    return m_horizSmplNum;
}

int LiDAR::getCurType()
{
    return m_initType;
}

static float ReverseFloat(const float inFloat)
{
    float retVal;
    char *floatToConvert = (char*)& inFloat;
    char *returnFloat = (char*)& retVal;

    // swap the bytes into a temporary buffer
    returnFloat[0] = floatToConvert[3];
    returnFloat[1] = floatToConvert[2];
    returnFloat[2] = floatToConvert[1];
    returnFloat[3] = floatToConvert[0];

    return retVal;
}

float LiDAR::depthFromNDC(int x, int y, float screenX, float screenY) {
    if (x >= s_camParams.width) {
        x = s_camParams.width - 1;
    }
    if (y >= s_camParams.height) {
        y = s_camParams.height - 1;
    }

    float xNorm = (float)x / (s_camParams.width - 1);
    float yNorm = (float)y / (s_camParams.height - 1);
    float normScreenX = abs(2 * xNorm - 1);
    float normScreenY = abs(2 * yNorm - 1);

    float ncX = normScreenX * s_camParams.ncWidth / 2;
    float ncY = normScreenY * s_camParams.ncHeight / 2;

    //Distance to near clip (hypotenus)
    float d2nc = sqrt(s_camParams.nearClip * s_camParams.nearClip + ncX * ncX + ncY * ncY);

    //depth value in normalized device coordinates (NDC)
    float ndc = m_depthMap[y * s_camParams.width + x];

    //Conversion from ndc to depth in camera coordinates
    float depth = d2nc / ndc;
    float depthDivisor = (s_camParams.nearClip * depth) / (2 * s_camParams.farClip);
    depth = depth / (1 + depthDivisor);

    return depth;
}

static bool isPositionOnScreen(float screenX, float screenY) {
    bool onScreen = true;
    if (screenX < 0 || screenY < 0 || screenX > 1 || screenY > 1) {
        onScreen = false;
    }
    return onScreen;
}

float LiDAR::getDepthFromScreenPos(float screenX, float screenY) {
    float depth;
    float halfW = 0.5 / s_camParams.width;
    float halfH = 0.5 / s_camParams.height;
    bool interpolated = false;
    if (screenX > halfW && screenX < 1 - halfW
        && screenY > halfH && screenY < 1 - halfH) {
        //Need to do -0.5 as indexing starts at 0
        float x = screenX * s_camParams.width - 0.5;
        float y = screenY * s_camParams.height - 0.5;

        int x0 = (int)floor(x);
        int x1 = (int)ceil(x);
        int y0 = (int)floor(y);
        int y1 = (int)ceil(y);

        float d00 = depthFromNDC(x0, y0);
        float d01 = depthFromNDC(x0, y1);
        float d10 = depthFromNDC(x1, y0);
        float d11 = depthFromNDC(x1, y1);

        //Normalize x/y to be between 0 and 1 to simplify interpolation
        float normX = (float)x - x0;
        float normY = (float)y - y0;

        //Bilinear interpolation
        //TODO: Would be better to use depth/stencil buffer to only interpolate on pixels of same object
        bool outsideThreshold = false;
        float minDepth = std::min(d00, std::min(d01, std::min(d10, d11)));
        float maxDepth = std::max(d00, std::max(d01, std::max(d10, d11)));
        if (maxDepth > minDepth * 1.08) {
            outsideThreshold = true;
        }
        if (!outsideThreshold) {
            interpolated = true;
            depth = (1 - normX)*(1 - normY)*d00 + normX * (1 - normY)*d10 + (1 - normX)*normY*d01 + normX * normY*d11;
        }
    }

    if (!interpolated) {
        //Pixels are 0 indexed
        int x = (int)floor(screenX * s_camParams.width);
        int y = (int)floor(screenY * s_camParams.height);

        depth = depthFromNDC(x, y, screenX, screenY);
    }

    if (LIDAR_GAUSSIAN_NOISE) {
        float before = depth;
        depth += s_nDist(s_rng);

        /*std::ostringstream oss1;
        oss1 << "Depth before/after dist: " << before << ", " << depth;
        std::string str1 = oss1.str();
        log(str1, true);*/
    }
    return depth;
}

Vector3 LiDAR::adjustEndCoord(Vector3 pos, Vector3 relPos) {
    float scrX, scrY;
    //Use this function over native function as native function fails at edges of screen
    Eigen::Vector2f uv = get_2d_from_3d(Eigen::Vector3f(pos.x, pos.y, pos.z));

    scrX = uv(0);
    scrY = uv(1);

    float screenX = scrX;// *width;
    float screenY = scrY;// *height;

    if (isPositionOnScreen(screenX, screenY)) {
        //It is expected for some LiDAR positions to be out of screen bounds
        log("Screen position is out of bounds.");
    }
    else {
        float depth = getDepthFromScreenPos(screenX, screenY);

        float originalDepth = sqrt(relPos.x * relPos.x + relPos.y * relPos.y + relPos.z * relPos.z);
        float multiplier = depth / originalDepth;

        /*std::ostringstream oss1;
        oss1 << "\nAdjust depth ScreenX: " << screenX << " screenY: " << screenY << 
            "\nAdjust depth NormScreenX: " << normScreenX << " NormScreenY: " << normScreenY <<
            "\nAdjust depth ncX: " << ncX << " ncY: " << ncY <<
            "\nAdjust depth near_clip: " << s_camParams.nearClip << " d2nc: " << d2nc <<
            "\nAdjust depth X: " << x << " Y: " << y <<
            "\noriginalDepth: " << originalDepth << "depth: " << depth << 
            "\nmultiplier: " << multiplier;
        std::string str1 = oss1.str();
        log(str1);*/
        relPos.x = relPos.x * multiplier;
        relPos.y = relPos.y * multiplier;
        relPos.z = relPos.z * multiplier;
    }
    return relPos;
}

Vector3 LiDAR::get3DFromDepthTarget(Vector3 target, Eigen::Vector2f target2D){
    Vector3 unitVec;
    float distance = sqrt(SYSTEM::VDIST2(s_camParams.pos.x, s_camParams.pos.y, s_camParams.pos.z, target.x, target.y, target.z));
    unitVec.x = (target.x - s_camParams.pos.x) / distance;
    unitVec.y = (target.y - s_camParams.pos.y) / distance;
    unitVec.z = (target.z - s_camParams.pos.z) / distance;

    float depth = getDepthFromScreenPos(target2D(0), target2D(1));

    //Depth is already in relative coordinates
    Vector3 depthEndCoord;
    depthEndCoord.x = unitVec.x * depth;
    depthEndCoord.y = unitVec.y * depth;
    depthEndCoord.z = unitVec.z * depth;

    //To convert from world coordinates to GTA vehicle coordinates (where y axis is forward)
    Vector3 vec_cam_coord = convertCoordinateSystem(depthEndCoord, cameraForwardVec, cameraRightVec, cameraUpVec);

    /*std::ostringstream oss2;
    oss2 << "***unitVec is: " << unitVec.x << ", " << unitVec.y << ", " << unitVec.z <<
    "\n depthEndCoord is: " << depthEndCoord.x << ", " << depthEndCoord.y << ", " << depthEndCoord.z <<
    "\nvec is: " << vec.x << ", " << vec.y << ", " << vec.z <<
    "\n s_camParams.pos is: " << s_camParams.pos.x << ", " << s_camParams.pos.y << ", " << s_camParams.pos.z;
    std::string str = oss2.str();
    log(str);*/
    return vec_cam_coord;
}

//Add a point hit to the entityID
void LiDAR::addToHitEntities(const Eigen::Vector2f &target2D) {
    //Will convert to the nearest pixel
    //Need to do -0.5 as indexing starts at 0
    int x = int(target2D(0) * s_camParams.width - 0.5);
    int y = int(target2D(1) * s_camParams.height - 0.5);

    int entityID = int(m_pInstanceSeg[y * s_camParams.width + x]);

    if (entityID == 0) return;

    if (m_entitiesHit->find(entityID) != m_entitiesHit->end()) {
        HitLidarEntity* hitEnt = m_entitiesHit->at(entityID);
        hitEnt->pointsHit++;
    }
    else {
        Vector3 forwardVector;
        Vector3 rightVector;
        Vector3 upVector;
        Vector3 position;
        ENTITY::GET_ENTITY_MATRIX(entityID, &forwardVector, &rightVector, &upVector, &position); //Blue or red pill
        position = subtractVector(position, s_camParams.pos);
        HitLidarEntity* hitEnt = new HitLidarEntity(forwardVector, position);
        m_entitiesHit->insert(std::pair<int, HitLidarEntity*>(entityID, hitEnt));
    }
}

void LiDAR::GenerateSinglePoint(float phi, float theta, float* p)
{
    if (m_pointsHit >= MAX_POINTS) {
        log("WARNING: MAX NUMBER OF POINTS REACHED! INCREASE MAX_POINTS\n", true);
    }
    BOOL isHit = false;
    Entity hitEntity;
    Vector3 target, endCoord, surfaceNorm;
    int raycast_handle;
    Eigen::Vector2f target2D;
    float phi_rad = phi * D2R, theta_rad = theta * D2R;

    endCoord.x = -m_maxRange * sin(phi_rad) * sin(theta_rad);	//rightward(east) is positive
    endCoord.y = m_maxRange * sin(phi_rad) * cos(theta_rad);	//forward(north) is positive
    endCoord.z = m_maxRange * cos(phi_rad);						//upward(up) is positive

    target.x = m_rotDCM[0] * endCoord.x + m_rotDCM[1] * endCoord.y + m_rotDCM[2] * endCoord.z + s_camParams.pos.x;
    target.y = m_rotDCM[3] * endCoord.x + m_rotDCM[4] * endCoord.y + m_rotDCM[5] * endCoord.z + s_camParams.pos.y;
    target.z = m_rotDCM[6] * endCoord.x + m_rotDCM[7] * endCoord.y + m_rotDCM[8] * endCoord.z + s_camParams.pos.z;

    if (USE_RAYCASTING) {
        //options: -1=everything
        //New function is called _START_SHAPE_TEST_RAY
        raycast_handle = WORLDPROBE::_CAST_RAY_POINT_TO_POINT(s_camParams.pos.x, s_camParams.pos.y, s_camParams.pos.z, target.x, target.y, target.z, -1, m_lidarVehicle, native_param);

        //New function is called GET_SHAPE_TEST_RESULT
        WORLDPROBE::_GET_RAYCAST_RESULT(raycast_handle, &isHit, &endCoord, &surfaceNorm, &hitEntity);
    }

    //The 2D screen coords of the target
    //This is what should be used for sampling depth map as endCoord will not hit same points as depth map
    target2D = get_2d_from_3d(Eigen::Vector3f(target.x, target.y, target.z));

    if (GENERATE_2D_POINTMAP) {
        *(m_lidar2DPoints + 2 * m_beamCount) = target2D(0);
        *(m_lidar2DPoints + 2 * m_beamCount + 1) = target2D(1);
        ++m_beamCount;
    }

    bool targetOnScreen = isPositionOnScreen(target2D(0), target2D(1));
    if (targetOnScreen) {
        Hit2DDepth hitDepth;
        hitDepth.target = target;
        hitDepth.target2D = target2D;
        hitDepth.groundDist = -1;
        if (OUTPUT_DEPTH_STATS) {
            float groundZ;
            GAMEPLAY::GET_GROUND_Z_FOR_3D_COORD(endCoord.x, endCoord.y, endCoord.z, &(groundZ), 0);
            hitDepth.groundDist = endCoord.z - groundZ;
        }
        float rayDist = sqrt(SYSTEM::VDIST2(s_camParams.pos.x, s_camParams.pos.y, s_camParams.pos.z, endCoord.x, endCoord.y, endCoord.z));
        hitDepth.rayCastDepth = rayDist;
        m_hitDepthPoints.push_back(hitDepth);

        Vector3 vec_cam_coord = get3DFromDepthTarget(target, target2D);

        /*std::ostringstream oss2;
        oss2 << "***vec_cam_coord is: " << vec_cam_coord.x << ", " << vec_cam_coord.y << ", " << vec_cam_coord.z;
        std::string str = oss2.str();
        log(str, true);*/

        float newDistance = sqrt(SYSTEM::VDIST2(0, 0, 0, vec_cam_coord.x, vec_cam_coord.y, vec_cam_coord.z));
        if (newDistance <= MAX_LIDAR_DIST) {
            //Note: The y/x axes are changed to conform with KITTI velodyne axes
            *p = vec_cam_coord.y;
            *(p + 1) = -vec_cam_coord.x;
            *(p + 2) = vec_cam_coord.z;
            *(p + 3) = 0;
            *(p + 4) = 0;
            *(p + 5) = 0;
            *(p + 6) = 0;
            //Need to do -0.5 as indexing starts at 0
            int i = floor(target2D(0) * s_camParams.width - 0.5);
            int j = floor(target2D(1) * s_camParams.height - 0.5);

            //*(p + 3) = m_pInstanceSeg[s_camParams.width * j + i];//We don't have the entityID if we're using the depth map
            int entityID = int(m_pInstanceSeg[s_camParams.width * j + i]);
            int ownVehicleID= PED::GET_VEHICLE_PED_IS_IN(PLAYER::PLAYER_PED_ID(), false);

            /* ----------------------------------------------------- NO USE CASE: ENTITY ID -----------------------------------------------------*/
            //Intensity value will be 0 for every point.
            if (entityID != ownVehicleID) {
                *(p + 3) = entityID;
            }else{
                *(p + 3) = 0;
            }
           
            /*  -------------------------------------------------------------------------------------------------------------------------------------------*/
             
            /* ----------------------------------------------------- USE CASE: IDEAL  DATASET -------------------------------------------------------------*/
            //Intensity value will be 1 for every object of the 'Car' class (ideal segmentation).

            Hash model = ENTITY::GET_ENTITY_MODEL(entityID); //Obtain vehicle model 

            //Get the model string, convert it to lowercase then find it in lookup table
            std::string modelString = VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(model);
            std::string before = modelString;
            std::transform(modelString.begin(), modelString.end(), modelString.begin(), ::tolower);
            std::string type = "Unknown";
            modelString.erase(remove_if(modelString.begin(), modelString.end(), [](char c) { return !isalpha(c); }), modelString.end());
            auto search = m_vLookupLidar.find(modelString);
            if (search != m_vLookupLidar.end()) { //model found
                type = search->second;
            }
            else { //model not found
                *(p + 4) = 999.99;
            }

            if (type == "Car" && entityID != ownVehicleID) { //if the model is a 'Car' and not the player's vehicle, intensity is 1
                *(p + 4) = 1.0;
            }
            else { //model is not a 'Car' or points are from player's own vehicle
                *(p + 4) = 0.0;
            }
            /*  -------------------------------------------------------------------------------------------------------------------------------------------*/


            /* ----------------------------------------------------- USE CASE: ZERO INTENSITY DATASET -----------------------------------------------------*/
            // Every point of the pointcloud has 0 intensity.
            *(p + 5) = 0.0;
            /*  -------------------------------------------------------------------------------------------------------------------------------------------*/
            

            /* ----------------------------------------------------------- USE CASE: VELOCITY DATASET ------------------------------------------------------*/
            // Obtain relative velocity in every point of the point cloud.
            /* :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: PLAYER ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: */
                                                                    /* :::::: PLAYER VELOCITY :::::: */
            //Obtain the velocity vector of the vehicle the player is using.
            Vector3 velocity_player = ENTITY::GET_ENTITY_SPEED_VECTOR(ownVehicleID, false);
            float InstantaneousSpeedPlayer = ENTITY::GET_ENTITY_SPEED(ownVehicleID);

                                                                   /* :::::: PLAYER COORDINATES :::::: */
            //Obtain the coordinates of the vehicle the player is using.
            Vector3 player_coords = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(ownVehicleID, 0.0, 0.0, 0.0);

            /* :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: OBJECT ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: */

                                                                    /* :::::: OBJECT VELOCITY :::::: */
            //Obtain the velocity vector of the vehicle the player is using.
            Vector3 object_vel_vect = ENTITY::GET_ENTITY_SPEED_VECTOR(entityID, false);
            float InstantaneousSpeedObject = ENTITY::GET_ENTITY_SPEED(entityID);

            bool isPedEnt = ENTITY::IS_ENTITY_A_PED(entityID) && PED::GET_PED_TYPE(entityID) != 28;
            bool isVehEnt = ENTITY::IS_ENTITY_A_VEHICLE(entityID);
            bool stationaryObjt;

            bool stationaryVehicle = VEHICLE::IS_VEHICLE_STOPPED(entityID);
            bool stationaryPed = PED::IS_PED_STOPPED(entityID);

            if (isPedEnt || isVehEnt) {
                if (stationaryPed || stationaryVehicle) {
                    stationaryObjt = true;
                }
                else{
                    stationaryObjt = false;
                }
            }
            else{
                  stationaryObjt = false;
            }

             /* :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: POINT ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: */
                                                                /* :::::: GROUND POINT COORDINATES :::::: */
             //Convert the camera value to the ground point in world coordinates. Requires axis transformation.

             Vector3 cameravalue;
             cameravalue.x = -vec_cam_coord.x;
             cameravalue.y = vec_cam_coord.y;
             cameravalue.z = vec_cam_coord.z;

             //PreSIL function to convert camera coordinates to world coordinates.
             Vector3 ground_point_coordinates = camToWorld(cameravalue, cameraForwardVec, cameraRightVec, cameraUpVec);

             /* :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: LIDAR ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: */
                                                                    /* :::::: LIDAR POSITION :::::: */
             //Obtain ground coordinates for LiDAR placement and create a new position vector. World coordinates have different depth due to
             //vehicle or pedestrian.
             float groundZ_player;
             GAMEPLAY::GET_GROUND_Z_FOR_3D_COORD(player_coords.x, player_coords.y, player_coords.z, &(groundZ_player), 0);
             Vector3 PlayerPoint;
             PlayerPoint.x = player_coords.x + 0;
             PlayerPoint.y = player_coords.y + 0;
             PlayerPoint.z = groundZ_player + 1.73;

                                                                /* :::::: GROUND ECHO ANGLE :::::: */
            //Obtain the relative position of the ground point. Thus, obtaining the distance from the vehicle to the ground point.
            Vector3 VectorDeltaPos;
            VectorDeltaPos.x = PlayerPoint.x - ground_point_coordinates.x;
            VectorDeltaPos.y = PlayerPoint.y - ground_point_coordinates.y;
            VectorDeltaPos.z = PlayerPoint.z - ground_point_coordinates.z;

            //Calculate the distance from the Z-axis (up) to the X-Y plane. 
            float xydistance = sqrt(pow(VectorDeltaPos.x, 2) + pow(VectorDeltaPos.y, 2));

            //Obtain the elevation angle
            float elevationval = atan2(VectorDeltaPos.z, xydistance);
            float elevationtoDegree = elevationval * 180 / PI; /* DEBUG ONLY*/


                                                                    /* :::::: DIRECTION :::::: */
            // Obtain direction only on objects that are moving
            if ((entityID != 0) && (entityID != ownVehicleID) && (stationaryObjt == false)) {
                /* ------------------------------------ POSITION  ------------------------------------ */
                // Collect object and player vehicle coordinates

                Vector3 object_coords = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(entityID, 0.0, 0.0, 0.0);

                Vector3 world_coord_diff;
                world_coord_diff.x = object_coords.x - player_coords.x;
                world_coord_diff.y = object_coords.y - player_coords.y;
                world_coord_diff.z = object_coords.z - player_coords.z;

                float norm_world_coords = sqrt(pow(world_coord_diff.x, 2) + pow(world_coord_diff.y, 2) + pow(world_coord_diff.z, 2));

                /* ------------------------------------  VELOCITY ------------------------------------ */
                // Obtain object and player vehicle velocity vectors

                // VELOCITY
                Vector3 object_vel_vect = ENTITY::GET_ENTITY_SPEED_VECTOR(entityID, false);

                Vector3 object_rel_vel_vect;
                object_rel_vel_vect.x = object_vel_vect.x - velocity_player.x;
                object_rel_vel_vect.y = object_vel_vect.y - velocity_player.y;
                object_rel_vel_vect.z = object_vel_vect.z - velocity_player.z;

                /* ------------------------------------  GRADIENT TO DIRECTION  ------------------------------------ */
                float InstantaneousSpeed = sqrt(pow(object_rel_vel_vect.x, 2) + pow(object_rel_vel_vect.y, 2) + pow(object_rel_vel_vect.z, 2));

                float CurrentDist = norm_world_coords;
                float t =1; //scale factor 1 for simplicity

                Vector3 New_Positionplayer;
                New_Positionplayer.x = player_coords.x + velocity_player.x * t;
                New_Positionplayer.y = player_coords.y + velocity_player.y * t;
                New_Positionplayer.z = player_coords.z + velocity_player.z * t;

                Vector3 New_PositionObject;
                New_PositionObject.x = object_coords.x + object_vel_vect.x * t;
                New_PositionObject.y = object_coords.y + object_vel_vect.y * t;
                New_PositionObject.z = object_coords.z + object_vel_vect.z * t;

                Vector3 New_Distance2;
                New_Distance2.x = New_Positionplayer.x - New_PositionObject.x;
                New_Distance2.y = New_Positionplayer.y - New_PositionObject.y;
                New_Distance2.z = New_Positionplayer.z - New_PositionObject.z;

                float FuturePos = sqrt(pow(New_Distance2.x, 2) + pow(New_Distance2.y, 2) + pow(New_Distance2.z, 2));

                float RelativeSpeed ;
                if (FuturePos > CurrentDist) { //departing
                    RelativeSpeed = InstantaneousSpeed;
                }
                else if (FuturePos == CurrentDist) { //same speed
                    RelativeSpeed = 0;
                } 
                else { //approaching
                    RelativeSpeed = -InstantaneousSpeed;
                }

                ///* ---------------------------------------- MEASURED SPEED ---------------------------------------- */
                //Cosine effect error of the velocity on the point.
                float velocityMeasuredLidar = RelativeSpeed  * cos(elevationval);

                *(p + 6) = velocityMeasuredLidar;
            }
            else if (((entityID == 0) || (stationaryObjt == true)) && (entityID != ownVehicleID)) {
                /* ---------------------------------------- VELOCITY ------------------------------------------*/
                //Get the player vehicle velocity vector [Vx,Vy,Vz] through a native function.

                //Create a ground velocity vector. As there are no natives to acquire point velocity (only for entitys),
                //manually create a vector with 0 velocity in each component.
                Vector3 ground_velocity;
                ground_velocity.x = 0;
                ground_velocity.y = 0;
                ground_velocity.z = 0;

                //Relative velocity can be infered by subtracting each component
                Vector3 GroundRelativeVelocity;
                GroundRelativeVelocity.x = ground_velocity.x - velocity_player.x;
                GroundRelativeVelocity.y = ground_velocity.y - velocity_player.y;
                GroundRelativeVelocity.z = ground_velocity.z - velocity_player.z;



                /* ---------------------------------------- MEASURED SPEED ---------------------------------------- */
                //Cosine effect error of the velocity on the point.
                float InstantaneousSpeed = sqrt(pow(GroundRelativeVelocity.x, 2) + pow(GroundRelativeVelocity.y, 2) + pow(GroundRelativeVelocity.z, 2));

                /* ---------------------------------------- MEASURED SPEED ---------------------------------------- */
                //Cosine effect error of the velocity on the point.
                float velocityMeasuredLidar = -InstantaneousSpeed * cos(elevationval);

                *(p + 6) = velocityMeasuredLidar;
            }
            /*  -------------------------------------------------------------------------------------------------------------------------------------------*/

            ++m_pointsHit;
            ++m_depthMapPoints;

            addToHitEntities(target2D);
        }
    }

    if (USE_RAYCASTING && isHit) {
        int entityID = 0;
        if ((ENTITY::IS_ENTITY_A_PED(hitEntity) && PED::GET_PED_TYPE(hitEntity) != 28) //PED_TYPE 28 are animals
            || ENTITY::IS_ENTITY_A_VEHICLE(hitEntity)) {
            entityID = hitEntity;
        }

        Vector3 vec;
        vec.x = endCoord.x - s_camParams.pos.x;
        vec.y = endCoord.y - s_camParams.pos.y;
        vec.z = endCoord.z - s_camParams.pos.z;

        //To convert from world coordinates to GTA vehicle coordinates (where y axis is forward)
        Vector3 vec_cam_coord = convertCoordinateSystem(vec, cameraForwardVec, cameraRightVec, cameraUpVec);

        if (!targetOnScreen) {
            //Note: The y/x axes are changed to conform with KITTI velodyne axes
            *p = vec_cam_coord.y;
            *(p + 1) = -vec_cam_coord.x;
            *(p + 2) = vec_cam_coord.z;
            *(p + 3) = entityID;//This is the entityID (Only non-zero for pedestrians and vehicles)
            ++m_pointsHit;

            float* pUpdatedPC = m_updatedPointCloud + (m_updatedPointCount * FLOATS_PER_POINT);
            *pUpdatedPC = vec_cam_coord.y;
            *(pUpdatedPC + 1) = -vec_cam_coord.x;
            *(pUpdatedPC + 2) = vec_cam_coord.z;
            *(pUpdatedPC + 3) = entityID;//This is the entityID (Only non-zero for pedestrians and vehicles)
            ++m_updatedPointCount;
        }

        if (OUTPUT_RAYCAST_POINTS) {
            //Note: The y/x axes are changed to conform with KITTI velodyne axes
            float* ptr = m_pRaycastPointCloud + (m_raycastPoints * FLOATS_PER_POINT);
            *ptr = vec_cam_coord.y;
            *(ptr + 1) = -vec_cam_coord.x;
            *(ptr + 2) = vec_cam_coord.z;
            *(ptr + 3) = entityID;//This is the entityID (Only non-zero for pedestrians and vehicles)
            ++m_raycastPoints;
        }

        //Likely just for testing purposes
        if (OUTPUT_ADJUSTED_POINTS) {
            //To convert from world coordinates to GTA vehicle coordinates (where y axis is forward)
            vec_cam_coord = convertCoordinateSystem(vec, cameraForwardVec, cameraRightVec, cameraUpVec);

            vec_cam_coord = adjustEndCoord(endCoord, vec_cam_coord);

            float distance = sqrt(SYSTEM::VDIST2(0, 0, 0, vec_cam_coord.x, vec_cam_coord.y, vec_cam_coord.z));
            if (distance <= MAX_LIDAR_DIST) {
                //Note: The y/x axes are changed to conform with KITTI velodyne axes
                *(p + 4) = vec_cam_coord.y;
                *(p + 5) = -vec_cam_coord.x;
                *(p + 6) = vec_cam_coord.z;
                *(p + 7) = 65535;//This is the entityID (Only non-zero for pedestrians and vehicles)
                ++m_pointsHit;
            }
        }

        //Can be used to get general outline of some objects with raycasting
        //WARNING: NOT ALL VEHICLES ARE HIT WITH RAYCASTING
        if (OBTAIN_RAY_POINTS_HIT) {
            if (m_entitiesHit->find(entityID) != m_entitiesHit->end()) {
                HitLidarEntity* hitEnt = m_entitiesHit->at(entityID);
                hitEnt->pointsHit++;
                Vector3 vecFromObjCenter = subtractVector(vec, hitEnt->position);
                float forwardFromObjCenter = vecFromObjCenter.x * hitEnt->forward.x + vecFromObjCenter.y * hitEnt->forward.y + vecFromObjCenter.z * hitEnt->forward.z;
                if (forwardFromObjCenter > hitEnt->maxFront) hitEnt->maxFront = forwardFromObjCenter;
                if (forwardFromObjCenter < hitEnt->maxBack) hitEnt->maxBack = forwardFromObjCenter;
            }
            else {
                Vector3 forwardVector;
                Vector3 rightVector;
                Vector3 upVector;
                Vector3 position;
                ENTITY::GET_ENTITY_MATRIX(entityID, &forwardVector, &rightVector, &upVector, &position); //Blue or red pill
                position = subtractVector(position, s_camParams.pos);
                HitLidarEntity* hitEnt = new HitLidarEntity(forwardVector, position);
                m_entitiesHit->insert(std::pair<int, HitLidarEntity*>(entityID, hitEnt));
            }
        }
    }

#ifdef DEBUG_LOG
    printf("\nDEBUG_LOG: function: %s", __FUNCTION__);
    printf("\ntheta=%f, endcoord:x=%f, y=%f, z=%f", __FUNCTION__, theta, endCoord.x, endCoord.y, endCoord.z);
#endif //DEBUG_LOG

#ifdef DEBUG_GRAPHICS_LIDAR
    //GRAPHICS::DRAW_BOX(endCoord.x - 0.05, endCoord.y - 0.05, endCoord.z - 0.05, endCoord.x + 0.05, endCoord.y + 0.05, endCoord.z + 0.05, 0, 255, 0, 255);
    GRAPHICS::DRAW_LINE(endCoord.x - 0.03, endCoord.y - 0.03, endCoord.z - 0.03, endCoord.x + 0.03, endCoord.y + 0.03, endCoord.z + 0.03, 255, 255, 255, 255);
#endif //DEBUG_GRAPHICS_LIDAR
}

void LiDAR::GenerateHorizPointClouds(float phi, float *p)
{
    int i, j;
    float theta = 0.0, quaterion[4];
    calcDCM();

    //Right side:
    theta = m_horizRiLimit;
    for (j = 0; j < m_horizSmplNum; j++)
    {
        if (theta < 360.0 - m_horizResolu)
            theta = m_horizRiLimit + j * m_horizResolu;
        else
            break;
        GenerateSinglePoint(phi, theta, p + (m_pointsHit * FLOATS_PER_POINT));
    }
    //Left side:
    theta = theta - 360.0;
    for (i = 0; i < m_horizSmplNum - j; i++)
    {
        if (theta < m_horizLeLimit - m_horizResolu)
            theta = 0.0 + i * m_horizResolu;
        else
            break;
        GenerateSinglePoint(phi, theta, p + (m_pointsHit * FLOATS_PER_POINT));
    }
}

void LiDAR::calcDCM()
{
    ENTITY::GET_ENTITY_QUATERNION(m_lidarVehicle, &m_quaterion[0], &m_quaterion[1], &m_quaterion[2], &m_quaterion[3]);
    //m_quaterion: R - coord spins to b - coord
    float q00 = m_quaterion[3] * m_quaterion[3], q11 = m_quaterion[0] * m_quaterion[0], q22 = m_quaterion[1] * m_quaterion[1], q33 = m_quaterion[2] * m_quaterion[2];
    float q01 = m_quaterion[3] * m_quaterion[0], q02 = m_quaterion[3] * m_quaterion[1], q03 = m_quaterion[3] * m_quaterion[2], q12 = m_quaterion[0] * m_quaterion[1];
    float q13 = m_quaterion[0] * m_quaterion[2], q23 = m_quaterion[1] * m_quaterion[2];

    //convert b-vector to R-vector, CbR
    m_rotDCM[0] = q00 + q11 - q22 - q33;
    m_rotDCM[1] = 2 * (q12 - q03);
    m_rotDCM[2] = 2 * (q13 + q02);
    m_rotDCM[3] = 2 * (q12 + q03);
    m_rotDCM[4] = q00 - q11 + q22 - q33;
    m_rotDCM[5] = 2 * (q23 - q01);
    m_rotDCM[6] = 2 * (q13 - q02);
    m_rotDCM[7] = 2 * (q23 + q01);
    m_rotDCM[8] = q00 - q11 - q22 + q33;
}

void LiDAR::updateCurrentPosition(Vector3 cameraForwardVector, Vector3 cameraRightVector, Vector3 cameraUpVector) {
    cameraForwardVec = cameraForwardVector;
    cameraRightVec = cameraRightVector;
    cameraUpVec = cameraUpVector;
}


void LiDAR::VehicleLookUpTable() {
    if (!m_vLookupInitLidar) {
        std::string translationFile = std::string(getenv("DEEPGTAV_DIR")) + "\\ObjectDet\\vehicle_labels.csv";
        std::ifstream inFile(translationFile);
        std::string line;
        log("Translation file:", true);
        log(translationFile, true);
        while (std::getline(inFile, line)) // read whole line into line
        {
            std::istringstream iss(line); // string stream
            std::string model;
            std::string vehicleType;
            std::getline(iss, model, ','); // read first part up to comma, ignore the comma
            std::string before = model;
            model.erase(remove_if(model.begin(), model.end(), [](char c) { return !isalpha(c); }), model.end());
            iss >> vehicleType; // read the second part
            m_vLookupLidar.insert(std::pair< std::string, std::string>(model, vehicleType));

            //Modelstrings seem to be missing last few letters on occasion (this should fix that)
            if (model.size() >= 2) {
                model = model.substr(0, model.size() - 1);
                m_vLookupLidar.insert(std::pair< std::string, std::string>(model, vehicleType));
            }
            if (model.size() >= 2) {
                model = model.substr(0, model.size() - 1);
                m_vLookupLidar.insert(std::pair< std::string, std::string>(model, vehicleType));
            }
            if (model.size() >= 2) {
                model = model.substr(0, model.size() - 1);
                m_vLookupLidar.insert(std::pair< std::string, std::string>(model, vehicleType));
            }
        }
        m_vLookupInitLidar = true;
    }
}