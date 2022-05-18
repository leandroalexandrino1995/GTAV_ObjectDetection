#define NOMINMAX

#include "ObjectDetection.h"
#include "..\ObjectDetIncludes.h"
#include <Windows.h>
#include <time.h>
#include <fstream>
#include <string>
#include <sstream>
#include "Functions.h"
#include "Constants.h"
#include <Eigen/Core>
#include <sstream>
#include "lodepng.h"

#include "LiDAR.h"

ObjectDetection::ObjectDetection()
{
}

ObjectDetection::~ObjectDetection()
{
}

//Global variable for storing camera parameters
CamParams s_camParams;

const float VERT_CAM_FOV = 59; //In degrees
                               //Need to input the vertical FOV with GTA functions.
                               //90 degrees horizontal (KITTI) corresponds to 59 degrees vertical (https://www.gtaall.com/info/fov-calculator.html).
const float HOR_CAM_FOV = 90; //In degrees

//Known stencil types
const int STENCIL_TYPE_DEFAULT = 0;//Ground, buildings, etc...
const int STENCIL_TYPE_NPC = 1;
const int STENCIL_TYPE_VEHICLE = 2;
const int STENCIL_TYPE_VEGETATION = 3;
const int STENCIL_TYPE_FLOOR = 4;//Seems to be floors and some boulevards
const int STENCIL_TYPE_SKY = 7;
const int STENCIL_TYPE_UNDERGROUND_ENTRANCE = 8;
const int STENCIL_TYPE_SELF = 129;
const int STENCIL_TYPE_OWNCAR = 130;
const std::vector<int> KNOWN_STENCIL_TYPES = { STENCIL_TYPE_DEFAULT, STENCIL_TYPE_NPC, STENCIL_TYPE_VEHICLE, STENCIL_TYPE_VEGETATION, STENCIL_TYPE_FLOOR, STENCIL_TYPE_SKY, STENCIL_TYPE_SELF, STENCIL_TYPE_OWNCAR, STENCIL_TYPE_UNDERGROUND_ENTRANCE };

const int PEDESTRIAN_CLASS_ID = 10;
const int CAR_CLASS_ID = 0;

void ObjectDetection::initCollection(UINT camWidth, UINT camHeight, bool exportEVE, int startIndex) {
    if (m_initialized) {
        return;
    }
    m_eve = exportEVE;
    instance_index = startIndex;

    ped = PLAYER::PLAYER_PED_ID();
    m_ownVehicle = PED::GET_VEHICLE_PED_IS_IN(ped, false);
    m_vehicle = m_ownVehicle;
    setOwnVehicleObject();

    char temp[] = "%06d";
    char strComp[sizeof temp + 100];
    sprintf(strComp, temp, instance_index);
    instance_string = strComp;
    s_camParams.width = (int)camWidth;
    s_camParams.height = (int)camHeight;
    //LOG(LL_ERR, "Testing4");

    //Need to set camera params
    s_camParams.init = false;
    camera = CAM::GET_RENDERING_CAM();
    //Create camera intrinsics matrix
    calcCameraIntrinsics();
    pointclouds = true;
    collectTracking = false;

    //Export directory
    log("Before getting export dir");
    baseFolder = std::string(getenv("DEEPGTAV_EXPORT_DIR")) + "\\";
    CreateDirectory(baseFolder.c_str(), NULL);
    if (exportEVE) {
        baseFolder += "eve\\";
    }
    if (collectTracking) {
        baseFolder += "tracking\\";
    }
    else {
        baseFolder += "object\\";
    }
    log("After getting export dir");
    CreateDirectory(baseFolder.c_str(), NULL);
    m_timeTrackFile = baseFolder + "\\TimeAnalysis.txt";
    m_usedPixelFile = baseFolder + "\\UsedPixels.txt";
    log("After getting export dir2");

    //Overwrite previous time analysis file so it is empty
    FILE* f = fopen(m_timeTrackFile.c_str(), "w");
    std::ostringstream oss;
    oss << "Mean err, var, avg speed, avg dist";
    oss << "\nResults are in metres. Frames attempted to capture at 10 Hz.";
    std::string str = oss.str();
    fprintf(f, str.c_str());
    fprintf(f, "\n");
    fclose(f);

    //Overwrite previous stencil pixel used file so it is empty
    f = fopen(m_usedPixelFile.c_str(), "w");
    std::ostringstream oss1;
    oss1 << "Unused pixels, index, series (if tracking)";
    std::string str1 = oss1.str();
    fprintf(f, str1.c_str());
    fprintf(f, "\n");
    fclose(f);
    log("Before initVehicleLookup");

    initVehicleLookup();
    //Setup LiDAR before collecting
    setupLiDAR();
    m_initialized = true;
}

//Set own object info for exporting position_world
void ObjectDetection::setOwnVehicleObject() {
    Vector3 min, max;
    Hash model = ENTITY::GET_ENTITY_MODEL(m_ownVehicle);
    Vector3 dim = getVehicleDims(m_ownVehicle, model, min, max);
    m_ownVehicleObj = ObjEntity(m_ownVehicle);

    //Fill out info for own car
    m_ownVehicleObj.objType = "Car";

    float kittiHeight = 2 * dim.z;
    float kittiWidth = 2 * dim.x;
    float kittiLength = 2 * dim.y;
    m_ownVehicleObj.width = kittiWidth;
    m_ownVehicleObj.height = kittiHeight;
    m_ownVehicleObj.length = kittiLength;

    Vector3 position;
    position.x = 0;
    position.y = 0;
    position.z = 0;
    m_ownVehicleObj.location = position;
    m_ownVehicleObj.rotation_y = -1;
    m_ownVehicleObj.alpha = -1;

    BBox2D bbox2d;
    bbox2d.bottom = -1;
    bbox2d.top = -1;
    bbox2d.left = -1;
    bbox2d.right = -1;
    m_ownVehicleObj.bbox2d = bbox2d;

    m_ownVehicleObj.truncation = -1;
    m_ownVehicleObj.occlusion = -1;
    m_ownVehicleObj.modelString = VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(model);

    m_ownVehicleObj.speed = -1;
    m_ownVehicleObj.roll = -1;
    m_ownVehicleObj.pitch = -1;

    m_ownVehicleObj.pointsHit2D = -1;
    m_ownVehicleObj.pointsHit3D = -1;
}

//For updating all depth/stencil related variables when depth/stencil buffer are one frame after game functions
FrameObjectInfo ObjectDetection::setDepthAndStencil(bool prevDepth, float* pDepth, uint8_t* pStencil) {
    if (prevDepth) {
        m_pDepth = pDepth;
        m_pStencil = pStencil;
    }
    else {
        setFilenames();
    }

    if (lidar_initialized) setDepthBuffer(prevDepth);
    if (lidar_initialized) setStencilBuffer();

    if (prevDepth) {
        if (lidar_initialized) printSegImage();
        if (lidar_initialized) outputOcclusion();
        if (lidar_initialized) outputUnusedStencilPixels();
    }

    return m_curFrame;
}

FrameObjectInfo ObjectDetection::generateMessage(float* pDepth, uint8_t* pStencil, int entityID) {
    //LOG(LL_ERR, "Depth data generate: ", pDepth[0], pDepth[1], pDepth[2], pDepth[3], pDepth[4], pDepth[5], pDepth[6], pDepth[7]);
    m_pDepth = pDepth;
    m_pStencil = pStencil;
    m_vPerspective = entityID;
    if (entityID != -1) {
        m_vehicle = entityID;
    }
    else {
        m_vehicle = m_ownVehicle;
    }

    //TODO pass this through
    bool depthMap = true;

    setIndex();
    setPosition();
    outputRealSpeed();
    setDepthAndStencil();
    //Need to set peds list first for integrating peds on bikes
    setPedsList();
    setVehiclesList();
    setSpeed();
    setYawRate();
    setTime();
    setFocalLength();
    log("After focalLength");

    //Update 2D bboxes, and create segmentation of stencil values
    processSegmentation2D();
    processSegmentation3D();
    processOcclusion();

    if (pointclouds && lidar_initialized) collectLiDAR();
    update3DPointsHit();

    if (depthMap && lidar_initialized) printSegImage();
    log("After printSeg");
    if (depthMap && lidar_initialized) outputOcclusion();
    log("After output occlusion");
    if (depthMap && lidar_initialized) outputUnusedStencilPixels();
    log("After output unused stencil");

    setGroundPlanePoints();

    return m_curFrame;
}

//Returns the angle between a relative position vector and the forward vector (rotated about up axis)
float ObjectDetection::observationAngle(Vector3 position) {
    float x1 = m_camRightVector.x;
    float y1 = m_camRightVector.y;
    float z1 = m_camRightVector.z;
    float x2 = position.x;
    float y2 = position.y;
    float z2 = position.z;
    float xn = m_camUpVector.x;
    float yn = m_camUpVector.y;
    float zn = m_camUpVector.z;

    float dot = x1 * x2 + y1 * y2 + z1 * z2;
    float det = x1 * y2*zn + x2 * yn*z1 + xn * y1*z2 - z1 * y2*xn - z2 * yn*x1 - zn * y1*x2;
    float observationAngle = atan2(det, dot);

    if (DEBUG_LOGGING) {
        std::ostringstream oss;
        oss << "Forward is: " << x1 << ", " << y1 << ", " << z1 <<
            "\nNormal is: " << x2 << ", " << y2 << ", " << z2 <<
            "\nPosition is: " << position.x << ", " << position.y << ", " << position.z << " and angle is: " << observationAngle;
        std::string str = oss.str();
        log(str);
    }

    return observationAngle;
}

void ObjectDetection::drawVectorFromPosition(Vector3 vector, int blue, int green) {
    GRAPHICS::DRAW_LINE(s_camParams.pos.x, s_camParams.pos.y, s_camParams.pos.z, vector.x * 1000 + s_camParams.pos.x, vector.y * 1000 + s_camParams.pos.y, vector.z * 1000 + s_camParams.pos.z, 0, green, blue, 200);
    WAIT(0);
}

//Saves the position and vectors of the capture vehicle
void ObjectDetection::setPosition() {
    //NOTE: The forward and right vectors are swapped (compared to native function labels) to keep consistency with coordinate system
    if (m_eve) {
        ENTITY::GET_ENTITY_MATRIX(m_vehicle, &vehicleForwardVector, &vehicleRightVector, &vehicleUpVector, &currentPos); //Blue or red pill

        /*LOG(LL_ERR, "Eve Forward vector: ", m_camForwardVector.x, " Y: ", m_camForwardVector.y, " Z: ", m_camForwardVector.z);
        LOG(LL_ERR, "Forward vector: ", vehicleForwardVector.x, " Y: ", vehicleForwardVector.y, " Z: ", vehicleForwardVector.z);
        LOG(LL_ERR, "Right vector: ", vehicleRightVector.x, " Y: ", vehicleRightVector.y, " Z: ", vehicleRightVector.z);
        LOG(LL_ERR, "Up vector: ", vehicleUpVector.x, " Y: ", vehicleUpVector.y, " Z: ", vehicleUpVector.z);
        LOG(LL_ERR, "Cam Forward vector: ", m_camForwardVector.x, " Y: ", m_camForwardVector.y, " Z: ", m_camForwardVector.z);
        LOG(LL_ERR, "Cam Right vector: ", m_camRightVector.x, " Y: ", m_camRightVector.y, " Z: ", m_camRightVector.z);
        LOG(LL_ERR, "Cam Up vector: ", m_camUpVector.x, " Y: ", m_camUpVector.y, " Z: ", m_camUpVector.z);
        LOG(LL_ERR, "Curr position: ", s_camParams.pos.x, " Y: ", s_camParams.pos.y, " Z: ", s_camParams.pos.z);
        LOG(LL_ERR, "Theta: ", s_camParams.theta.x, " Y: ", s_camParams.theta.y, " Z: ", s_camParams.theta.z);*/

        float ogThetaZ = tan(-vehicleForwardVector.x / vehicleForwardVector.y) * 180 / PI;
        float newThetaZ = tan(-m_camForwardVector.x / m_camForwardVector.y) * 180 / PI;
        float ogThetaZ2 = atan2(-vehicleForwardVector.x, vehicleForwardVector.y) * 180 / PI;
        float newThetaZ2 = atan2(-m_camForwardVector.x, m_camForwardVector.y) * 180 / PI;
        float ogThetaX2 = atan2(vehicleForwardVector.z, sqrt(pow(vehicleForwardVector.y, 2) + pow(vehicleForwardVector.x, 2))) * 180 / PI;
        float newThetaX2 = atan2(m_camForwardVector.z, sqrt(pow(m_camForwardVector.y, 2) + pow(m_camForwardVector.x, 2))) * 180 / PI;
        float ogThetaX = tan(vehicleForwardVector.z / sqrt(pow(vehicleForwardVector.y, 2) + pow(vehicleForwardVector.x, 2))) * 180 / PI;
        float newThetaX = tan(m_camForwardVector.z / sqrt(pow(m_camForwardVector.y, 2) + pow(m_camForwardVector.x, 2))) * 180 / PI;
        //LOG(LL_ERR, "Theta og/new Z: ", ogThetaZ, ", ", newThetaZ, " og/new Z2: ", ogThetaZ2, ", ", newThetaZ2, " og, new X: ", ogThetaX, ", ", newThetaX, " og, new ThetaX2: ", ogThetaX2, ", ", newThetaX2);

        float ogThetaY2 = atan2(vehicleRightVector.z, sqrt(pow(vehicleRightVector.y, 2) + pow(vehicleRightVector.x, 2))) * 180 / PI;
        float newThetaY2 = atan2(-m_camRightVector.z, sqrt(pow(m_camRightVector.y, 2) + pow(m_camRightVector.x, 2))) * 180 / PI;
        //LOG(LL_ERR, "Theta og/new Y2: ", ogThetaY2, ", ", newThetaY2);
        s_camParams.theta.x = newThetaX2;
        s_camParams.theta.y = newThetaY2;
        s_camParams.theta.z = newThetaZ2;
        //LOG(LL_ERR, "Theta: ", s_camParams.theta.x, " Y: ", s_camParams.theta.y, " Z: ", s_camParams.theta.z);
    }
    else {
        //If not eve, the camera and vehicle are aligned by pausing and flushing the buffers
        ENTITY::GET_ENTITY_MATRIX(m_vehicle, &m_camForwardVector, &m_camRightVector, &m_camUpVector, &currentPos);
        ENTITY::GET_ENTITY_MATRIX(m_vehicle, &vehicleForwardVector, &vehicleRightVector, &vehicleUpVector, &currentPos); //Blue or red pill
    }

    m_curFrame.position = currentPos;

    m_curFrame.roll = atan2(-vehicleRightVector.z, sqrt(pow(vehicleRightVector.y, 2) + pow(vehicleRightVector.x, 2)));
    m_curFrame.pitch = atan2(-vehicleForwardVector.z, sqrt(pow(vehicleForwardVector.y, 2) + pow(vehicleForwardVector.x, 2)));

    //Should use atan2 over gameplay heading
    //float heading = GAMEPLAY::GET_HEADING_FROM_VECTOR_2D(vehicleForwardVector.x, vehicleForwardVector.y);
    m_curFrame.heading = atan2(vehicleForwardVector.y, vehicleForwardVector.x);

    m_curFrame.forwardVec = vehicleForwardVector;
    m_curFrame.upVec = vehicleUpVector;
    m_curFrame.rightVec = vehicleRightVector;
    m_curFrame.camPos = s_camParams.pos;

    //Check if we see it (not occluded)
    Vector3 min, max, offcenter;
    Hash model = ENTITY::GET_ENTITY_MODEL(m_vehicle);
    GAMEPLAY::GET_MODEL_DIMENSIONS(model, &min, &max);
    Vector3 kittiWorldPos = correctOffcenter(currentPos, min, max, vehicleForwardVector, vehicleRightVector, vehicleUpVector, offcenter);
    m_curFrame.kittiWorldPos = kittiWorldPos;
}

void ObjectDetection::setSpeed() {
    m_curFrame.speed = ENTITY::GET_ENTITY_SPEED(m_vehicle);
}

void ObjectDetection::setYawRate() {
    Vector3 rates = ENTITY::GET_ENTITY_ROTATION_VELOCITY(m_vehicle);
    m_curFrame.yawRate = rates.z*180.0 / 3.14159265359;
}

void ObjectDetection::setTime() {
    m_curFrame.timeHours = TIME::GET_CLOCK_HOURS();
}

//Cycle through 8 corners of bbox and see if the ray makes it to or past this point
bool ObjectDetection::hasLOSToEntity(Entity entityID, Vector3 position, Vector3 dim, Vector3 forwardVector, Vector3 rightVector, Vector3 upVector, bool useOrigin, Vector3 origin) {
    Vector3 oPos;
    if (useOrigin) {
        oPos = origin;
    }
    else {
        oPos = s_camParams.pos;
    }

    for (int right = -1; right <= 1; right += 2) {
        for (int forward = -1; forward <= 1; forward += 2) {
            for (int up = -1; up <= 1; up += 2) {
                Vector3 pos;
                pos.x = position.x + forward * dim.y*forwardVector.x + right * dim.x*rightVector.x + up * dim.z*upVector.x;
                pos.y = position.y + forward * dim.y*forwardVector.y + right * dim.x*rightVector.y + up * dim.z*upVector.y;
                pos.z = position.z + forward * dim.y*forwardVector.z + right * dim.x*rightVector.z + up * dim.z*upVector.z;

                Vector3 relPos;
                relPos.x = pos.x - oPos.x;
                relPos.y = pos.y - oPos.y;
                relPos.z = pos.z - oPos.z;

                BOOL isHit;
                Entity hitEntity;
                Vector3 target, endCoord, surfaceNorm;
                target.x = relPos.x * 200 + pos.x;
                target.y = relPos.y * 200 + pos.y;
                target.z = relPos.z * 200 + pos.z;

                //options: -1=everything
                //New function is called _START_SHAPE_TEST_RAY
                int raycast_handle = WORLDPROBE::_CAST_RAY_POINT_TO_POINT(oPos.x, oPos.y, oPos.z, target.x, target.y, target.z, -1, m_vehicle, 7);

                //New function is called GET_SHAPE_TEST_RESULT
                WORLDPROBE::_GET_RAYCAST_RESULT(raycast_handle, &isHit, &endCoord, &surfaceNorm, &hitEntity);

                float distance = sqrt(SYSTEM::VDIST2(oPos.x, oPos.y, oPos.z, pos.x, pos.y, pos.z));
                float rayDistance = sqrt(SYSTEM::VDIST2(oPos.x, oPos.y, oPos.z, endCoord.x, endCoord.y, endCoord.z));

                if (!isHit || rayDistance > distance) {
                    return true;
                }
            }
        }
    }
    return false;
}

BBox2D ObjectDetection::BBox2DFrom3DObject(Vector3 position, Vector3 dim, Vector3 forwardVector, Vector3 rightVector, Vector3 upVector, bool &success, float &truncation) {
    //Adjust position back to middle of the object for calculating 2D bounding box (Kitti has it at bottom)
    position.z += dim.z;

    BBox2D bbox2d;
    bbox2d.left = 1.0;// width;
    bbox2d.right = 0.0;
    bbox2d.top = 1.0;// height;
    bbox2d.bottom = 0.0;

    for (int right = -1; right <= 1; right += 2) {
        for (int forward = -1; forward <= 1; forward += 2) {
            for (int up = -1; up <= 1; up += 2) {
                Vector3 pos;
                pos.x = position.x + forward * dim.y*forwardVector.x + right * dim.x*rightVector.x + up * dim.z*upVector.x;
                pos.y = position.y + forward * dim.y*forwardVector.y + right * dim.x*rightVector.y + up * dim.z*upVector.y;
                pos.z = position.z + forward * dim.y*forwardVector.z + right * dim.x*rightVector.z + up * dim.z*upVector.z;

                float screenX, screenY;
//This function always returns false, do not worry about return value
bool success = GRAPHICS::_WORLD3D_TO_SCREEN2D(pos.x, pos.y, pos.z, &screenX, &screenY);

std::ostringstream oss2;
oss2 << "\nnew ScreenX: " << screenX << " ScreenY: " << screenY;
std::string str2 = oss2.str();
log(str2);

//Calculate with eigen if off-screen
Eigen::Vector3f pt(pos.x, pos.y, pos.z);
if (screenX < 0 || screenX > 1 || screenY < 0 || screenY > 1) {
    Eigen::Vector2f uv = get_2d_from_3d(pt);
    screenX = uv(0);
    screenY = uv(1);
}

if (CORRECT_2D_POINTS_BEHIND_CAMERA) {
    //Corrections for points which are behind camera
    Vector3 relativePos;
    relativePos.x = pos.x - s_camParams.pos.x;
    relativePos.y = pos.y - s_camParams.pos.y;
    relativePos.z = pos.z - s_camParams.pos.z;
    relativePos = convertCoordinateSystem(relativePos, m_camForwardVector, m_camRightVector, m_camUpVector);

    //If behind camera update left/right bounds to reflect its position
    if (relativePos.y < 0) {
        if (relativePos.x > 0) {
            screenX = 1.0;
        }
        else {
            screenX = 0.0;
        }
    }
}

//Update if value outside current box
if (screenX < bbox2d.left) bbox2d.left = screenX;
if (screenX > bbox2d.right) bbox2d.right = screenX;
if (screenY < bbox2d.top) bbox2d.top = screenY;
if (screenY > bbox2d.bottom) bbox2d.bottom = screenY;
            }
        }
    }

    //Calculate truncation
    float absL = std::max<float>(0, bbox2d.left);
    float absR = std::min<float>(1, bbox2d.right);
    float absT = std::max<float>(0, bbox2d.top);
    float absB = std::min<float>(1, bbox2d.bottom);
    float areaInside = (absR - absL) * (absB - absT);
    float areaTotal = (bbox2d.right - bbox2d.left) * (bbox2d.bottom - bbox2d.top);
    truncation = 1 - (areaInside / areaTotal);

    //Set bbox boundaries
    bbox2d.left = std::max(0.0f, bbox2d.left);
    bbox2d.right = std::min(1.0f, bbox2d.right);
    bbox2d.top = std::max(0.0f, bbox2d.top);
    bbox2d.bottom = std::min(1.0f, bbox2d.bottom);

    //Entire object is out of bounds - do not count
    if (bbox2d.left == bbox2d.right || bbox2d.top == bbox2d.bottom) {
        success = false;
        return bbox2d;
    }

    std::ostringstream oss2;
    oss2 << "BBox left: " << bbox2d.left << " right: " << bbox2d.right << " top: " << bbox2d.top << " bot: " << bbox2d.bottom << std::endl <<
        "PosX: " << bbox2d.posX() << " PosY: " << bbox2d.posY() << " Width: " << bbox2d.width() << " Height: " << bbox2d.height();
    std::string str2 = oss2.str();
    log(str2);
    return bbox2d;
}

bool ObjectDetection::checkDirection(Vector3 unit, Vector3 point, Vector3 min, Vector3 max) {
    float dotPoint = dotProd(point, unit);
    float dotMax = dotProd(max, unit);
    float dotMin = dotProd(min, unit);

    if ((dotMax <= dotPoint && dotPoint <= dotMin) ||
        (dotMax >= dotPoint && dotPoint >= dotMin)) {
        return true;
    }
    return false;
}

//Point and objPos should be in world coordinates
void ObjectDetection::setEntityBBoxParameters(ObjEntity* e) {

    //Added BBOX_ADJUSTMENT_FACTOR as detailed models sometimes go outside 3D bboxes
    Vector3 forward; forward.y = e->dim.y * BBOX_ADJUSTMENT_FACTOR; forward.x = 0; forward.z = 0;
    forward = convertCoordinateSystem(forward, e->yVector, e->xVector, e->zVector);

    Vector3 right; right.x = e->dim.x * BBOX_ADJUSTMENT_FACTOR; right.y = 0; right.z = 0;
    right = convertCoordinateSystem(right, e->yVector, e->xVector, e->zVector);

    Vector3 up; up.z = e->dim.z; up.x = 0; up.y = 0;
    up = convertCoordinateSystem(up, e->yVector, e->xVector, e->zVector);

    //position is given at bottom of bounding box (as per kitti)
    Vector3 objPos = e->worldPos;

    e->rearBotLeft.x = objPos.x - forward.x - right.x - up.x * (BBOX_ADJUSTMENT_FACTOR - 1);
    e->rearBotLeft.y = objPos.y - forward.y - right.y - up.y * (BBOX_ADJUSTMENT_FACTOR - 1);
    e->rearBotLeft.z = objPos.z - forward.z - right.z - up.z * (BBOX_ADJUSTMENT_FACTOR - 1);

    e->frontBotLeft.x = objPos.x + forward.x - right.x - up.x * (BBOX_ADJUSTMENT_FACTOR - 1);
    e->frontBotLeft.y = objPos.y + forward.y - right.y - up.y * (BBOX_ADJUSTMENT_FACTOR - 1);
    e->frontBotLeft.z = objPos.z + forward.z - right.z - up.z * (BBOX_ADJUSTMENT_FACTOR - 1);

    e->rearTopLeft.x = objPos.x - forward.x - right.x + 2 * up.x + up.x * BBOX_ADJUSTMENT_FACTOR;
    e->rearTopLeft.y = objPos.y - forward.y - right.y + 2 * up.y + up.y * BBOX_ADJUSTMENT_FACTOR;
    e->rearTopLeft.z = objPos.z - forward.z - right.z + 2 * up.z + up.z * BBOX_ADJUSTMENT_FACTOR;

    e->rearBotRight.x = objPos.x - forward.x + right.x - up.x * (BBOX_ADJUSTMENT_FACTOR - 1);
    e->rearBotRight.y = objPos.y - forward.y + right.y - up.y * (BBOX_ADJUSTMENT_FACTOR - 1);
    e->rearBotRight.z = objPos.z - forward.z + right.z - up.z * (BBOX_ADJUSTMENT_FACTOR - 1);

    e->rearMiddleLeft.x = objPos.x - forward.x - right.x + up.x;
    e->rearMiddleLeft.y = objPos.y - forward.y - right.y + up.y;
    e->rearMiddleLeft.z = objPos.z - forward.z - right.z + up.z;

    e->rearThirdLeft.x = objPos.x - forward.x - right.x + 2 / 3 * up.x;
    e->rearThirdLeft.y = objPos.y - forward.y - right.y + 2 / 3 * up.y;
    e->rearThirdLeft.z = objPos.z - forward.z - right.z + 2 / 3 * up.z;

    e->rearTopExactLeft.x = objPos.x - forward.x - right.x + 2 * up.x;
    e->rearTopExactLeft.y = objPos.y - forward.y - right.y + 2 * up.y;
    e->rearTopExactLeft.z = objPos.z - forward.z - right.z + 2 * up.z;

    e->u = getUnitVector(subtractVecs(e->frontBotLeft, e->rearBotLeft));
    e->v = getUnitVector(subtractVecs(e->rearTopLeft, e->rearBotLeft));
    e->w = getUnitVector(subtractVecs(e->rearBotRight, e->rearBotLeft));
}

//Return true if pixel (i,j) is inside the entity's unprocessed 2D bounding box
bool ObjectDetection::in2DBoxUnprocessed(const int &i, const int &j, ObjEntity* e) {
    if (i < e->bbox2dUnprocessed.left) return false;
    if (i > e->bbox2dUnprocessed.right) return false;
    if (j < e->bbox2dUnprocessed.top) return false;
    if (j > e->bbox2dUnprocessed.bottom) return false;

    return true;
}

//Point and objPos should be in world coordinates
bool ObjectDetection::in3DBox(Vector3 point, Vector3 objPos, Vector3 dim, Vector3 yVector, Vector3 xVector, Vector3 zVector) {
    Vector3 forward; forward.y = dim.y; forward.x = 0; forward.z = 0;
    forward = convertCoordinateSystem(forward, yVector, xVector, zVector);

    Vector3 right; right.x = dim.x; right.y = 0; right.z = 0;
    right = convertCoordinateSystem(right, yVector, xVector, zVector);

    Vector3 up; up.z = dim.z; up.x = 0; up.y = 0;
    up = convertCoordinateSystem(up, yVector, xVector, zVector);

    Vector3 rearBotLeft;
    rearBotLeft.x = objPos.x - forward.x - right.x - up.x;
    rearBotLeft.y = objPos.y - forward.y - right.y - up.y;
    rearBotLeft.z = objPos.z - forward.z - right.z - up.z;

    Vector3 frontBotLeft;
    frontBotLeft.x = objPos.x + forward.x - right.x - up.x;
    frontBotLeft.y = objPos.y + forward.y - right.y - up.y;
    frontBotLeft.z = objPos.z + forward.z - right.z - up.z;

    Vector3 rearTopLeft;
    rearTopLeft.x = objPos.x - forward.x - right.x + up.x;
    rearTopLeft.y = objPos.y - forward.y - right.y + up.y;
    rearTopLeft.z = objPos.z - forward.z - right.z + up.z;

    Vector3 rearBotRight;
    rearBotRight.x = objPos.x - forward.x + right.x - up.x;
    rearBotRight.y = objPos.y - forward.y + right.y - up.y;
    rearBotRight.z = objPos.z - forward.z + right.z - up.z;

    std::ostringstream oss2;
    oss2 << " rearBotLeft are: " << rearBotLeft.x << ", " << rearBotLeft.y << ", " << rearBotLeft.z;
    oss2 << "\nfrontBotLeft: " << frontBotLeft.x << ", " << frontBotLeft.y << ", " << frontBotLeft.z;
    oss2 << "\nrearTopLeft: " << rearTopLeft.x << ", " << rearTopLeft.y << ", " << rearTopLeft.z;
    oss2 << "\nrearBotRight: " << rearBotRight.x << ", " << rearBotRight.y << ", " << rearBotRight.z;
    oss2 << "\ndim: " << dim.x << ", " << dim.y << ", " << dim.z;
    std::string str2 = oss2.str();
    log(str2);

    Vector3 u = getUnitVector(subtractVecs(frontBotLeft, rearBotLeft));
    Vector3 v = getUnitVector(subtractVecs(rearTopLeft, rearBotLeft));
    Vector3 w = getUnitVector(subtractVecs(rearBotRight, rearBotLeft));

    if (!checkDirection(u, point, rearBotLeft, frontBotLeft)) return false;
    if (!checkDirection(v, point, rearBotLeft, rearTopLeft)) return false;
    if (!checkDirection(w, point, rearBotLeft, rearBotRight)) return false;

    return true;
}

//Takes in an entity and a point (in world coordinates) and returns true if the point resides within the
//entity's 3D bounding box.
//Note: Need to set the entity's parameters u,v,w, and rearBotLeft, etc...
bool ObjectDetection::in3DBox(ObjEntity* e, Vector3 point, bool &upperHalf) {
    upperHalf = false;
    if (checkDirection(e->v, point, e->rearMiddleLeft, e->rearTopExactLeft)) upperHalf = true;

    if (!checkDirection(e->u, point, e->rearBotLeft, e->frontBotLeft)) return false;
    if (!checkDirection(e->v, point, e->rearBotLeft, e->rearTopLeft)) return false;
    if (!checkDirection(e->w, point, e->rearBotLeft, e->rearBotRight)) return false;

    return true;
}

bool ObjectDetection::isPointOccluding(Vector3 worldPos, ObjEntity *e) {
    //Need to test in3DBox as stencil buffer goes through windows but depth buffer does not
    bool upperHalf;
    if (in3DBox(e, worldPos, upperHalf)) {
        return false;
    }

    float pointDist = sqrt(SYSTEM::VDIST2(s_camParams.pos.x, s_camParams.pos.y, s_camParams.pos.z, worldPos.x, worldPos.y, worldPos.z));
    float distObjCenter = sqrt(SYSTEM::VDIST2(s_camParams.pos.x, s_camParams.pos.y, s_camParams.pos.z, e->worldPos.x, e->worldPos.y, e->worldPos.z));

    //Point needs to be closer
    if (pointDist < distObjCenter) {
        float groundZ;
        GAMEPLAY::GET_GROUND_Z_FOR_3D_COORD(worldPos.x, worldPos.y, worldPos.z + 0.5, &(groundZ), 0);
        //Check it is not the ground in the image (or the ground is much higher/lower than the object)
        if ((groundZ + GROUND_POINT_MAX_DIST) < worldPos.z || s_camParams.pos.z > (e->worldPos.z + 4) || s_camParams.pos.z < (e->worldPos.z - 2)) {
            return true;
        }
    }

    return false;
}

//TODO Need to fix this now that we know stencil/depth buffers do not align
//The problem is that depth buffer hits vehicle windows.
//The stencil buffer goes through the vehicle windows and captures whatever is behind them.
//This makes it difficult for segmentation and 2D vs 3D segmentation will be different.
void ObjectDetection::processSegmentation2D() {
    /*for (int j = 0; j < s_camParams.height; ++j) {
        for (int i = 0; i < s_camParams.width; ++i) {
            uint8_t stencilVal = m_pStencil[j * s_camParams.width + i];

            if (stencilVal == STENCIL_TYPE_VEHICLE || stencilVal == STENCIL_TYPE_NPC) {
                processStencilPixel2D(stencilVal, j, i, xVectorCam, yVectorCam, zVectorCam);
            }
            else if (stencilVal == STENCIL_TYPE_OWNCAR) {
                addPointTo2DSegImages(i, j, m_ownVehicle);
            }
        }
    }

    processOverlappingPoints2D();*/
}

void ObjectDetection::processSegmentation3D() {
    //Converting vehicle dimensions from vehicle to world coordinates for offset position
    Vector3 worldX; worldX.x = 1; worldX.y = 0; worldX.z = 0;
    Vector3 worldY; worldY.x = 0; worldY.y = 1; worldY.z = 0;
    Vector3 worldZ; worldZ.x = 0; worldZ.y = 0; worldZ.z = 1;
    Vector3 xVectorCam = convertCoordinateSystem(worldX, m_camForwardVector, m_camRightVector, m_camUpVector);
    Vector3 yVectorCam = convertCoordinateSystem(worldY, m_camForwardVector, m_camRightVector, m_camUpVector);
    Vector3 zVectorCam = convertCoordinateSystem(worldZ, m_camForwardVector, m_camRightVector, m_camUpVector);

    //Create depth array to use later
    for (int j = 0; j < s_camParams.height; ++j) {
        for (int i = 0; i < s_camParams.width; ++i) {
            float ndc = m_pDepth[j * s_camParams.width + i];
            Vector3 relPos = depthToCamCoords(ndc, i, j);
            float distance = sqrt(SYSTEM::VDIST2(0, 0, 0, relPos.x, relPos.y, relPos.z));
            m_depthMat.at<float>(j, i) = distance;
        }
    }

    //Set the bounding box parameters (for reducing # of calculations per pixel)
    for (auto &entry : m_curFrame.vehicles) {
        setEntityBBoxParameters(&entry.second);
    }
    for (auto &entry : m_curFrame.peds) {
        setEntityBBoxParameters(&entry.second);
    }

    for (int j = 0; j < s_camParams.height; ++j) {
        for (int i = 0; i < s_camParams.width; ++i) {
            uint8_t stencilVal = m_pStencil[j * s_camParams.width + i];

            if (stencilVal == STENCIL_TYPE_OWNCAR) {
                addPointToSegImages(i, j, m_ownVehicle);
            }
            else {
                processStencilPixel3D(stencilVal, j, i, xVectorCam, yVectorCam, zVectorCam);
            }
        }
    }

    if (PROCESS_OVERLAPPING_POINTS) {
        processOverlappingPoints();
    }
}

//Goes through points which were in multiple 3D boxes (overlapping points)
//Uses opencv to flood fill all points for sets of entities which have overlapping points
//If filled areas have a unique entityID (other than the overlapping points), entire area gets set to the unique entityID
//TODO: Step 2: find contour with depth for areas which have more than a single unique entityID
void ObjectDetection::processOverlappingPoints() {
    //Create mask with only points from overlapping entities
    //Process one set of entity IDs at a time
    while (!m_overlappingPoints.empty()) {
        std::vector<ObjEntity*> objEntities = m_overlappingPoints.begin()->second;
        int ptIdx = m_overlappingPoints.begin()->first;

        int stencilType = STENCIL_TYPE_VEHICLE;
        if (objEntities[0]->objType == "Pedestrian") {
            stencilType = STENCIL_TYPE_NPC;
        }

        //Initialize mask
        cv::Mat allPointsMask = cv::Mat::zeros(cv::Size(s_camParams.width, s_camParams.height), CV_8U);

        //Create mask of all points from overlapping entities
        for (int j = 0; j < s_camParams.height; ++j) {
            for (int i = 0; i < s_camParams.width; ++i) {
                int idx = j * s_camParams.width + i;
                uint8_t stencilVal = m_pStencil[idx];

                if (stencilVal == stencilType) {
                    if (m_overlappingPoints.find(idx) != m_overlappingPoints.end()) {
                        //Add to mask
                        allPointsMask.at<uchar>(j, i) = 255;
                    }
                    else {
                        for (auto pObjEntity : objEntities) {
                            if (m_pInstanceSeg[idx] == pObjEntity->entityID) {
                                allPointsMask.at<uchar>(j, i) = 255;
                                break;
                            }
                        }
                    }
                }
            }
        }
        
        //Test by printing out image
        /*{
            std::string entitiesStr;
            for (auto pObjEntity : objEntities) {
                entitiesStr.append(std::to_string(pObjEntity->entityID));
                entitiesStr.append("-");
            }
            entitiesStr.append("allPointsMask");
            std::string filename = getStandardFilename(entitiesStr, ".png");
            cv::imwrite(filename, allPointsMask);
        }*/

        //Create individual segmented masks
        int floodVal = 1;
        for (int j = 0; j < s_camParams.height; ++j) {
            for (int i = 0; i < s_camParams.width; ++i) {
                if (allPointsMask.at<uchar>(j, i) == 255) {
                    cv::floodFill(allPointsMask, cv::Point(i,j), cv::Scalar(floodVal));
                    ++floodVal;
                }
            }
        }

        //Test by printing out image
        /*{
            std::string entitiesStr;
            for (auto pObjEntity : objEntities) {
                entitiesStr.append(std::to_string(pObjEntity->entityID));
                entitiesStr.append("-");
            }
            entitiesStr.append("floodFilledMask");
            std::string filename = getStandardFilename(entitiesStr, ".png");
            cv::imwrite(filename, allPointsMask);
        }*/

        //Check if each floodFill value only has singular points from one entity
        std::vector<int> floodFillEntities(floodVal - 1, 0);
        std::vector<bool> goodFloods(floodVal - 1, true);

        for (int j = 0; j < s_camParams.height; ++j) {
            for (int i = 0; i < s_camParams.width; ++i) {
                int curFloodVal = allPointsMask.at<uchar>(j, i);

                if (curFloodVal != 0) {
                    int idx = j * s_camParams.width + i;
                    int entityID = m_pInstanceSeg[idx];
                    
                    if (entityID != 0) {
                        int floodEntityID = floodFillEntities[curFloodVal - 1];

                        if (floodEntityID == 0) floodFillEntities[curFloodVal - 1] = entityID;
                        else if (entityID == floodEntityID) continue;
                        else goodFloods[curFloodVal - 1] = false;
                    }
                }
            }
        }

        //If yes, set all points in that segment mask to the corresponding entity
        for (int j = 0; j < s_camParams.height; ++j) {
            for (int i = 0; i < s_camParams.width; ++i) {
                int curFloodVal = allPointsMask.at<uchar>(j, i);

                if (curFloodVal != 0 && m_pInstanceSeg[j * s_camParams.width + i] == 0) {
                    if (goodFloods[curFloodVal - 1]) {
                        for (ObjEntity* objEnt : objEntities) {
                            if (objEnt->entityID == floodFillEntities[curFloodVal - 1]) {
                                addSegmentedPoint3D(i, j, objEnt);
                                //Also zero the mask pixel so we don't use it in the future
                                allPointsMask.at<uchar>(j, i) = 0;
                                break;
                            }
                        }
                    }
                    else {
                        int idx = j * s_camParams.width + i;
                        //Last resort just set the point to be the entity of the nearest 3D point
                        float dist = FLT_MAX;
                        ObjEntity* closestObj = NULL;
                        float ndc = m_pDepth[idx];
                        Vector3 relPos = depthToCamCoords(ndc, i, j);
                        for (auto pObjEntity : objEntities) {
                            float distToObj = sqrt(SYSTEM::VDIST2(pObjEntity->location.x, pObjEntity->location.y, pObjEntity->location.z, relPos.x, relPos.y, relPos.z));
                            if (distToObj < dist) {
                                dist = distToObj;
                                closestObj = pObjEntity;
                            }
                        }
                        addSegmentedPoint3D(i, j, closestObj);
                    }
                }
            }
        }

        //For testing print out indices which can't separate with flood fill so they can be visually inspected
        for (int i = 0; i < floodVal; ++i) {
            if (goodFloods[i] == false) {
                std::ostringstream oss2;
                oss2 << "**************Found bad flood at index: " << instance_index << " with floodval: " <<floodVal << " and i: " << i;
                std::string str = oss2.str();
                log(str, true);
            }
        }

        //Round 2: If a segment from above has more than two sure entities in it
        //Then try creating contours within this mask from the depth threshold
        //Separate then check if new segments only have one sure entity in them

        //Create an image with the depth values only where the mask is
        //Initialize depth mask
        //cv::Mat depthMasked;
        //m_depthMat.copyTo(depthMasked, allPointsMask);
        //depthMasked *= FLT_MAX / s_camParams.farClip;

        ////Test by printing out image
        //{
        //    std::string entitiesStr;
        //    for (auto pObjEntity : objEntities) {
        //    entitiesStr.append(std::to_string(pObjEntity->entityID));
        //    entitiesStr.append("-");
        //    }
        //    entitiesStr.append("depthMasked");
        //    std::string filename = getStandardFilename(entitiesStr, ".png");
        //    cv::imwrite(filename, depthMasked);
        //}

        std::ostringstream oss2;
        oss2 << "Overlapping points size: " << m_overlappingPoints.size();
        std::string str = oss2.str();
        log(str, true);
        
        //If point is still in overlapping points then remove it
        if (m_overlappingPoints.find(ptIdx) != m_overlappingPoints.end()) {
            m_overlappingPoints.erase(ptIdx);
        }

        allPointsMask.release();
    }

    //Reset the map once done processing
    m_overlappingPoints.clear();
}

std::vector<ObjEntity*> ObjectDetection::pointInside3DEntities(const Vector3 &worldPos, EntityMap* eMap, const bool &checkUpperVehicle, const uint8_t &stencilVal) {
    //Get vector of entities which point resides in their 3D box
    std::vector<ObjEntity*> pointEntities;
    for (auto &entry : *eMap) {
        ObjEntity* e = &(entry.second);
        bool upperHalf;
        bool isIn3DBox = in3DBox(e, worldPos, upperHalf);
        if (isIn3DBox) {
            //Add only pedestrian stencil types to pedestrian 3D bboxes
            //Add any points which are vehicle stencil type or
            //are in the upper half of the vehicle's 3D bounding box
            //This allows window points to be added for vehicles
            if (!checkUpperVehicle || stencilVal == STENCIL_TYPE_VEHICLE || upperHalf) {
                pointEntities.push_back(e);
            }
        }
    }

    return pointEntities;
}

//j is y coordinate (top=0), i is x coordinate (left = 0)
void ObjectDetection::processStencilPixel3D(const uint8_t &stencilVal, const int &j, const int &i,
                                          const Vector3 &xVectorCam, const Vector3 &yVectorCam, const Vector3 &zVectorCam) {
    float ndc = m_pDepth[j * s_camParams.width + i];
    Vector3 relPos = depthToCamCoords(ndc, i, j);
    Vector3 worldPos = convertCoordinateSystem(relPos, yVectorCam, xVectorCam, zVectorCam);
    worldPos.x += s_camParams.pos.x;
    worldPos.y += s_camParams.pos.y;
    worldPos.z += s_camParams.pos.z;

    //Obtain proper map for stencil type
    //Need to check all points for vehicles since depth map hits windows but
    //stencil buffer hits entities through windows
    EntityMap* eMap;
    bool checkUpperVehicle = false;
    if (stencilVal == STENCIL_TYPE_NPC) {
        eMap = &m_curFrame.peds;
    }
    else {
        eMap = &m_curFrame.vehicles;
        checkUpperVehicle = true;
    }

    //Check 2D boxes first for vehicle and pedestrian stencil pixels
    //Stencil goes through vehicle windows but depth buffer does not
    std::vector<ObjEntity*> pointEntities2D;
    if (stencilVal == STENCIL_TYPE_NPC || stencilVal == STENCIL_TYPE_VEHICLE) {
        for (auto &entry : *eMap) {
            ObjEntity* e = &(entry.second);
            if (in2DBoxUnprocessed(i, j, e)) {
                pointEntities2D.push_back(e);
            }
        }
        //If point only lies in one 2D bounding box then accept this entity as the true entity
        if (pointEntities2D.size() == 1) {
            addSegmentedPoint3D(i, j, pointEntities2D[0]);
            return;
        }
    }

    std::vector<ObjEntity*> pointEntities = pointInside3DEntities(worldPos, eMap, checkUpperVehicle, stencilVal);

    //All vehicle points should fall within a vehicle 3D bounding box
    //Pedestrians in vehicles may not since the windows are what the depth model hits
    //Try setting the pedestrian stencil type to a vehicle and checking again if this is the case
    if (pointEntities.empty() && stencilVal == STENCIL_TYPE_NPC) {
        eMap = &m_curFrame.vehicles;
        checkUpperVehicle = true;
        pointEntities = pointInside3DEntities(worldPos, eMap, checkUpperVehicle, STENCIL_TYPE_VEHICLE);
    }

    //3 choices, no, single, or multiple 3D box matches
    if (pointEntities.empty()) {
        //Should never hit here, point will not get added for outlier cases
    }
    else if (pointEntities.size() == 1) {
        addSegmentedPoint3D(i, j, pointEntities[0]);
    }
    else {
        //If the overlapping entities are all peds in the same vehicle
        //simply add it as it will just be set to the vehicle
        if (stencilVal == STENCIL_TYPE_NPC && pointEntities[0]->isPedInV) {
            int vEntityID = pointEntities[0]->vPedIsIn;
            bool allSameV = true;
            for (int i = 1; i < pointEntities.size(); ++i) {
                if (!pointEntities[i]->isPedInV || pointEntities[i]->vPedIsIn != pointEntities[0]->vPedIsIn) {
                    allSameV = false;
                    break;
                }
            }
            if (allSameV) {
                addSegmentedPoint3D(i, j, pointEntities[0]);
                return;
            }
        }

        //Index of point
        int idx = j * s_camParams.width + i;

        //Map should only hit each idx once, so no need for alternative if idx is found
        if (PROCESS_OVERLAPPING_POINTS) {
            if (m_overlappingPoints.find(idx) == m_overlappingPoints.end()) {
                m_overlappingPoints.insert(std::pair<int, std::vector<ObjEntity*>>(idx, pointEntities));
            }
            else {
                log("************************This should never be here!!!!!!!!!!!!!!!!!!!", true);
            }
        }
    }
}

//Add 2D point to an entity
void ObjectDetection::addSegmentedPoint3D(int i, int j, ObjEntity *e) {

    if (e->isPedInV) {
        for (auto &entry : m_curFrame.vehicles) {
            ObjEntity* entity = &(entry.second);
            if (entity->entityID == e->vPedIsIn) {
                e = entity;
            }
        }
    }

    /* TESTE */
    //List of trailers to be considered for towing purposes
    //std::list<std::string> list_of_trailers = { "TRAILERLARGE", "TRAILER","TRAILERS3", "DOCKTRAILER","TR2","TANKER","TRAILERL","ARMYTRAILER","TR3"};
    //Hash model = ENTITY::GET_ENTITY_MODEL(e->entityID);
    //std::string modelString = VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(model);
    //std::list<std::string>::iterator it;
    //it = std::find(list_of_trailers.begin(), list_of_trailers.end(), modelString);
    //if (it != list_of_trailers.end()) //It's a car
    //{
    //    Entity TruckAttached = ENTITY::GET_ENTITY_ATTACHED_TO(e->entityID);
    //    if (TruckAttached != 0) { // Detects if truck has a trailer
    //        e->entityID = TruckAttached;
    //    }
    //}
    /* FIM */

    if (i < e->bbox2d.left) e->bbox2d.left = i;
    if (i > e->bbox2d.right) e->bbox2d.right = i;
    if (j < e->bbox2d.top) e->bbox2d.top = j;
    if (j > e->bbox2d.bottom) e->bbox2d.bottom = j;
    ++e->pointsHit2D;

    //Remove from overlappingPoints if it gets added
    int idx = j * s_camParams.width + i;
    if (m_overlappingPoints.find(idx) != m_overlappingPoints.end()) {
        m_overlappingPoints.erase(idx);
    }

    addPointToSegImages(i, j, e->entityID);
}

void ObjectDetection::addPointToSegImages(int i, int j, int entityID) {
    //Index of point in all image buffers
    int idx = j * s_camParams.width + i;

    //instance seg is image with exact entityIDs
    m_pInstanceSeg[idx] = (uint32_t)entityID; 

    //RGB image is 3 bytes per pixel
    int segIdx = 3 * idx;
    uint8_t red = m_pStencilSeg[segIdx];
    uint8_t green = m_pStencilSeg[segIdx + 1];
    uint8_t blue = m_pStencilSeg[segIdx + 2];
    if (red == 0 && green == 0 && blue == 0) {
        int newVal = 47 * entityID; //Just to produce unique but different colours
        red = (newVal + 13 * entityID) % 255;
        green = (newVal / 255) % 255;
        blue = newVal % 255;
    }
    else {
        red = 255;
        green = 255;
        blue = 255;
    }
    uint8_t* p = m_pStencilSeg + segIdx;
    *p = red;
    *(p + 1) = green;
    *(p + 2) = blue;
}

//process occlusion after all 2D points are segmented
void ObjectDetection::processOcclusion() {
    //Converting vehicle dimensions from vehicle to world coordinates for offset position
    Vector3 worldX; worldX.x = 1; worldX.y = 0; worldX.z = 0;
    Vector3 worldY; worldY.x = 0; worldY.y = 1; worldY.z = 0;
    Vector3 worldZ; worldZ.x = 0; worldZ.y = 0; worldZ.z = 1;
    Vector3 xVectorCam = convertCoordinateSystem(worldX, m_camForwardVector, m_camRightVector, m_camUpVector);
    Vector3 yVectorCam = convertCoordinateSystem(worldY, m_camForwardVector, m_camRightVector, m_camUpVector);
    Vector3 zVectorCam = convertCoordinateSystem(worldZ, m_camForwardVector, m_camRightVector, m_camUpVector);

    for (auto &entry : m_curFrame.vehicles) {
        processOcclusionForEntity(&entry.second, xVectorCam, yVectorCam, zVectorCam);
    }
    for (auto &entry : m_curFrame.peds) {
        processOcclusionForEntity(&entry.second, xVectorCam, yVectorCam, zVectorCam);
    }
}

void ObjectDetection::processOcclusionForEntity(ObjEntity *e, const Vector3 &xVectorCam, const Vector3 &yVectorCam, const Vector3 &zVectorCam) {
    int occlusionPointCount = 0;

    for (int j = e->bbox2dUnprocessed.top; j < e->bbox2dUnprocessed.bottom; ++j) {
        for (int i = e->bbox2dUnprocessed.left; i < e->bbox2dUnprocessed.right; ++i) {

            int entityID = int(m_pInstanceSeg[j * s_camParams.width + i]);

            if (entityID != e->entityID) {
                uint8_t stencilVal = m_pStencil[j * s_camParams.width + i];
                float ndc = m_pDepth[j * s_camParams.width + i];
                Vector3 relPos = depthToCamCoords(ndc, i, j);
                Vector3 worldPos = convertCoordinateSystem(relPos, yVectorCam, xVectorCam, zVectorCam);
                worldPos.x += s_camParams.pos.x;
                worldPos.y += s_camParams.pos.y;
                worldPos.z += s_camParams.pos.z;

                if (stencilVal != STENCIL_TYPE_SKY && isPointOccluding(worldPos, e)) {
                    ++occlusionPointCount;
                    m_pOcclusionImage[j * s_camParams.width + i] = 255;
                }
            }
        }
    }

    float occtreshold = 1.0;
    
    int divisor = occlusionPointCount + e->pointsHit2D;
    if (divisor != 0) {
        occtreshold = (float)occlusionPointCount / (float)(divisor);
    }
    if (occtreshold < 0.2) {
        e->occlusion = 0;
    }
    else if (occtreshold < 0.6) {
        e->occlusion = 1;
    }
    else {
        e->occlusion = 2;
    }
    
}

//dim is in full width/height/length
static void updatePosition(float &origPos, float pedPos, float &origDim, float pedDim) {
    float diffPos = pedPos + pedDim / 2 - (origPos + origDim / 2);
    float diffNeg = pedPos - pedDim / 2 - (origPos - origDim / 2);
    if (diffPos > 0) { //If ped dim is outside original bike dimension
        origPos += diffPos / 2;
        origDim += diffPos;
    }
    if (diffNeg < 0) { //If ped dim is outside original bike dimension
        //If it is negative we need to add the position (since it is relative)
        //but subtract the dimension (really make it bigger since diffNeg < 0 if it is bigger)
        origPos += diffNeg / 2;
        origDim -= diffNeg;
    }
}

void ObjectDetection::getRollAndPitch(Vector3 rightVector, Vector3 forwardVector, Vector3 upVector, float &pitch, float &roll) {
    Vector3 kittiForwardVector = convertCoordinateSystem(forwardVector, m_camForwardVector, m_camRightVector, m_camUpVector);
    Vector3 kittiRightVector = convertCoordinateSystem(rightVector, m_camForwardVector, m_camRightVector, m_camUpVector);

    roll = atan2(-kittiRightVector.z, sqrt(pow(kittiRightVector.y, 2) + pow(kittiRightVector.x, 2)));
    pitch = atan2(-kittiForwardVector.z, sqrt(pow(kittiForwardVector.y, 2) + pow(kittiForwardVector.x, 2)));
}

Vector3 ObjectDetection::correctOffcenter(Vector3 position, Vector3 min, Vector3 max, Vector3 forwardVector, Vector3 rightVector, Vector3 upVector, Vector3 &offcenter) {
    //Amount dimensions are offcenter
    offcenter.x = (max.x + min.x) / 2;
    offcenter.y = (max.y + min.y) / 2;
    offcenter.z = min.z; //KITTI position is at object ground plane

    //Converting vehicle dimensions from vehicle to world coordinates for offset position
    Vector3 worldX; worldX.x = 1; worldX.y = 0; worldX.z = 0;
    Vector3 worldY; worldY.x = 0; worldY.y = 1; worldY.z = 0;
    Vector3 worldZ; worldZ.x = 0; worldZ.y = 0; worldZ.z = 1;
    Vector3 xVector = convertCoordinateSystem(worldX, forwardVector, rightVector, upVector);
    Vector3 yVector = convertCoordinateSystem(worldY, forwardVector, rightVector, upVector);
    Vector3 zVector = convertCoordinateSystem(worldZ, forwardVector, rightVector, upVector);
    Vector3 offcenterPosition = convertCoordinateSystem(offcenter, yVector, xVector, zVector);

    //Update object position to be consistent with KITTI (symmetrical dimensions except for z which is ground)
    position.x = position.x + offcenterPosition.x;
    position.y = position.y + offcenterPosition.y;
    position.z = position.z + offcenterPosition.z;

    return position;
}

void ObjectDetection::update3DPointsHit(ObjEntity* e) {
    //Checks to see if LiDAR hit entity e
    if (m_entitiesHit.find(e->entityID) != m_entitiesHit.end()) {
        HitLidarEntity* hitLidarEnt = m_entitiesHit[e->entityID];
        e->pointsHit3D = hitLidarEnt->pointsHit;
    }
    //Entities not found will have their 3D point count remain at zero
}

void ObjectDetection::update3DPointsHit() {
    //Update # of 2D and 3D pixels for each entity
    for (auto &entry : m_curFrame.vehicles) {
        update3DPointsHit(&entry.second);
    }
    for (auto &entry : m_curFrame.peds) {
        update3DPointsHit(&entry.second);
    }
}

bool ObjectDetection::getEntityVector(ObjEntity &entity, int entityID, Hash model, int classid, std::string type, std::string modelString, bool isPedInV, int vPedIsIn, bool &nearbyVehicle) {
    bool success = false;

    Vector3 FUR; //Front Upper Right
    Vector3 BLL; //Back Lower Left
    Vector3 dim; //Vehicle dimensions
    Vector3 upVector, rightVector, forwardVector, position; //Vehicle position
    Vector3 m_upVector, m_rightVector, m_forwardVector, m_position; //Own Vehicle position
    Vector3 min;
    Vector3 max;
    Vector3 speedVector;

    float heading, speed;

    ENTITY::GET_ENTITY_MATRIX(m_vehicle, &m_forwardVector, &m_rightVector, &m_upVector, &m_position); //Blue or red pill

    ENTITY::GET_ENTITY_MATRIX(entityID, &forwardVector, &rightVector, &upVector, &position); //Blue or red pill
    float distance = sqrt(SYSTEM::VDIST2(s_camParams.pos.x, s_camParams.pos.y, s_camParams.pos.z, position.x, position.y, position.z));
    if (nearbyVehicle) {
        //nearbyVehicle needs to be within range
        if (distance > SECONDARY_PERSPECTIVE_RANGE) {
            nearbyVehicle = false;
        }//nearby vehicle needs to be occupied
        else if (ONLY_OCCUPIED_VEHICLES && VEHICLE::IS_VEHICLE_SEAT_FREE(entityID, -1)) {
            nearbyVehicle = false;
        }
    }

    //Check if it is on screen
    bool isOnScreen = ENTITY::IS_ENTITY_ON_SCREEN(entityID);
    if (isOnScreen || nearbyVehicle || AUGMENT_ALL_VEHICLES_IN_RANGE) {
        if (isOnScreen || AUGMENT_ALL_VEHICLES_IN_RANGE) {
            success = true;
        }
        
        //Need to limit distance as pixels won't register entities past the far clip
        if (distance > s_camParams.farClip) return false;

        speed = ENTITY::GET_ENTITY_SPEED(entityID);

        
        GAMEPLAY::GET_MODEL_DIMENSIONS(model, &min, &max);

        //Need to adjust dimensions for pedestrians
        if (classid == PEDESTRIAN_CLASS_ID) {
            float groundZ;
            GAMEPLAY::GET_GROUND_Z_FOR_3D_COORD(position.x, position.y, position.z, &(groundZ), 0);
            float negZ = groundZ - position.z;

            //Pedestrians on balconies can cause problems
            if (negZ > -2.0 && negZ < 0) {
                min.z = groundZ - position.z;
            }

            if (SET_PED_BOXES) {
                min.x = -PED_BOX_WIDTH / 2.0f;
                max.x = PED_BOX_WIDTH / 2.0f;
                if (speed > 1) {
                    min.y = -PED_BOX_WALKING_LEN / 2.0f;
                    max.y = PED_BOX_WALKING_LEN / 2.0f;
                }
                else {
                    min.y = -PED_BOX_LENGTH / 2.0f;
                    max.y = PED_BOX_LENGTH / 2.0f;
                }
            }

            std::ostringstream oss2;
            oss2 << "***min: " << min.x << ", " << min.y << ", " << min.z <<
                "\nmax: " << max.x << ", " << max.y << ", " << max.z;
            std::string str = oss2.str();
            log(str);
        }

        //Calculate size
        dim.x = 0.5*(max.x - min.x);
        dim.y = 0.5*(max.y - min.y);
        dim.z = 0.5*(max.z - min.z);

        //TODO Remove these calculations and put x/y/zVector and m_camRight/Forward/UpVector in s_camParams
        //Converting vehicle dimensions from vehicle to world coordinates for offset position
        Vector3 worldX; worldX.x = 1; worldX.y = 0; worldX.z = 0;
        Vector3 worldY; worldY.x = 0; worldY.y = 1; worldY.z = 0;
        Vector3 worldZ; worldZ.x = 0; worldZ.y = 0; worldZ.z = 1;
        Vector3 xVector = convertCoordinateSystem(worldX, forwardVector, rightVector, upVector);
        Vector3 yVector = convertCoordinateSystem(worldY, forwardVector, rightVector, upVector);
        Vector3 zVector = convertCoordinateSystem(worldZ, forwardVector, rightVector, upVector);

        Vector3 offcenter;
        position = correctOffcenter(position, min, max, forwardVector, rightVector, upVector, offcenter);

        speedVector = ENTITY::GET_ENTITY_SPEED_VECTOR(entityID, false);
        if (speed > 0) {
            heading = GAMEPLAY::GET_HEADING_FROM_VECTOR_2D(speedVector.x - vehicleForwardVector.x, speedVector.y - vehicleForwardVector.y);
        }
        else {
            heading = GAMEPLAY::GET_HEADING_FROM_VECTOR_2D(forwardVector.x - vehicleForwardVector.x, forwardVector.y - vehicleForwardVector.y);
        }

        //Kitti dimensions
        float kittiHeight = 2 * dim.z;
        float kittiWidth = 2 * dim.x;
        float kittiLength = 2 * dim.y;
                    
        //Have only seen sitting people with height <= 1.0
        if (classid == PEDESTRIAN_CLASS_ID && kittiHeight <= 1.0) {
            type = "Person_sitting";
        }

        if (abs(offcenter.y) > 0.5 && classid == 0) {
            std::ostringstream oss2;
            oss2 << "Instance Index: " << instance_index << " Dimensions are: " << dim.x << ", " << dim.y << ", " << dim.z;
            oss2 << "\nMax: " << max.x << ", " << max.y << ", " << max.z;
            oss2 << "\nMin: " << min.x << ", " << min.y << ", " << min.z;
            oss2 << "\noffset: " << offcenter.x << ", " << offcenter.y << ", " << offcenter.z;
            std::string str2 = oss2.str();
            log(str2);
        }

        Vector3 relativePos;
        relativePos.x = position.x - s_camParams.pos.x;
        relativePos.y = position.y - s_camParams.pos.y;
        relativePos.z = position.z - s_camParams.pos.z;

        Vector3 kittiForwardVector = convertCoordinateSystem(forwardVector, m_camForwardVector, m_camRightVector, m_camUpVector);
        float rot_y = -atan2(kittiForwardVector.y, kittiForwardVector.x);

        relativePos = convertCoordinateSystem(relativePos, m_camForwardVector, m_camRightVector, m_camUpVector);

        //Update object position to be consistent with KITTI (symmetrical dimensions except for z which is ground)
        //relativePos.y = relativePos.y - CAM_OFFSET_FORWARD;
        //relativePos.z = relativePos.z - CAM_OFFSET_UP;

        //Convert to KITTI camera coordinates
        Vector3 kittiPos;
        kittiPos.x = relativePos.x;
        kittiPos.y = -relativePos.z;
        kittiPos.z = relativePos.y;

        //alpha is rot_y + tan^-1(z/x) + PI/2
        float beta_kitti = atan2(kittiPos.z, kittiPos.x);
        float alpha_kitti = rot_y + beta_kitti - PI / 2;

        log("After processBBox2D");
        bool foundPedOnBike = false;
        if (PROCESS_PEDS_ON_BIKES) {
            if (m_pedsInVehicles.find(entityID) != m_pedsInVehicles.end()) {
                std::vector<Ped> pedsOnV = m_pedsInVehicles[entityID];

                for (auto ped : pedsOnV) {
                    std::ostringstream oss;
                    oss << "Found ped on bike at index: " << instance_index;
                    std::string str = oss.str();
                    log(str, true);
                    log("Found ped on bike", true);
                    foundPedOnBike = true;
                    //Extend 3D/2D boxes with peds, change id in segmentation image

                    if (m_curFrame.peds.find(ped) != m_curFrame.peds.end()) {
                        ObjEntity pedO = m_curFrame.peds[ped];

                        float horizDist = sqrt(pow(pedO.location.x - kittiPos.x, 2) + pow(pedO.location.z - kittiPos.z, 2));
                        float vertDist = pedO.location.y - kittiPos.y;
                        if (horizDist < 0.5 && vertDist <= 2.0) {
                            foundPedOnBike = true;
                            m_curFrame.peds[ped].isPedInV = true;
                            m_curFrame.peds[ped].vPedIsIn = entityID;

                            //This method assume the pedestrian x/z coordinates are the same as the vehicles (only update relative height position)
                            kittiWidth = kittiWidth > pedO.width ? kittiWidth : pedO.width;
                            kittiLength = kittiLength > pedO.length ? kittiLength : pedO.length;
                            updatePosition(kittiPos.y, pedO.location.y, kittiHeight, pedO.height);

                            //This method would need to be changed to accommodate object coordinate system vs kitti coordinate system
                            /*updatePosition(kittiPos.x, pedO.location.x, kittiWidth, pedO.width);
                            updatePosition(kittiPos.y, pedO.location.y, kittiWidth, pedO.width);
                            updatePosition(kittiPos.z, pedO.location.z, kittiWidth, pedO.width);*/
                        }
                    }
                }
            }
            else if (TESTING_PEDS_ON_BIKES && classid == 1 || classid == 2 || classid == 3) { //bicycle, bike, or quadbike
                for (auto ped : m_curFrame.peds) {
                    ObjEntity pedO = ped.second;
                    float horizDist = sqrt(pow(pedO.location.x - kittiPos.x, 2) + pow(pedO.location.z - kittiPos.z, 2));
                    float vertDist = pedO.location.y - kittiPos.y;
                    if (horizDist < 0.5 && vertDist <= 2.0) {
                        m_curFrame.peds[ped.first].isPedInV = true;
                        m_curFrame.peds[ped.first].vPedIsIn = entityID;
                        foundPedOnBike = true;
                        std::ostringstream oss;
                        oss << "****************************Alternate Found ped on bike at index: " << instance_index;
                        std::string str = oss.str();
                        log(str, true);
                                    
                        //This method assume the pedestrian x/z coordinates are the same as the vehicles (only update relative height position)
                        kittiWidth = kittiWidth > pedO.width ? kittiWidth : pedO.width;
                        kittiLength = kittiLength > pedO.length ? kittiLength : pedO.length;
                        updatePosition(kittiPos.y, pedO.location.y, kittiHeight, pedO.height);

                        //This method would need to be changed to accommodate object coordinate system vs kitti coordinate system
                        /*updatePosition(kittiPos.x, pedO.location.x, kittiWidth, pedO.width);
                        updatePosition(kittiPos.y, pedO.location.y, kittiWidth, pedO.width);
                        updatePosition(kittiPos.z, pedO.location.z, kittiWidth, pedO.width);*/
                    }
                }
            }
        }
        log("After pedsonbikes");

        //Attempts to find bbox on screen
        float truncation = 0;
        BBox2D bbox2d;
        if (isOnScreen) {
            bbox2d = BBox2DFrom3DObject(position, dim, forwardVector, rightVector, upVector, success, truncation);
        }
        //stencil type for vehicles like cars, bikes...
        int stencilType = STENCIL_TYPE_VEHICLE;
        //Pedestrian classid
        if (classid == PEDESTRIAN_CLASS_ID) stencilType = STENCIL_TYPE_NPC; //For NPCs

        float roll = atan2(-rightVector.z, sqrt(pow(rightVector.y, 2) + pow(rightVector.x, 2)));
        float pitch = atan2(-forwardVector.z, sqrt(pow(forwardVector.y, 2) + pow(forwardVector.x, 2)));
        getRollAndPitch(rightVector, forwardVector, upVector, pitch, roll);
        //To prevent negative zeros
        if (abs(roll) <= 0.0001) roll = 0.0f;
        if (abs(pitch) <= 0.0001) pitch = 0.0f;

        entity.entityID = entityID;
        entity.classID = classid;
        entity.speed = speed;
        entity.heading = heading;

        entity.height = kittiHeight;
        entity.width = kittiWidth;
        entity.length = kittiLength;
        entity.dim = dim;

        entity.offcenter = offcenter;
        entity.location = kittiPos;
        entity.worldPos = position;

        entity.rotation_y = rot_y;
        entity.alpha = alpha_kitti;
        entity.distance = distance;

        //Blank bbox to be filled in later
        BBox2D bbox2dProcessed = BBox2D();
        entity.bbox2d = bbox2dProcessed;
                    
        entity.bbox2dUnprocessed.left = bbox2d.left * s_camParams.width;
        entity.bbox2dUnprocessed.top = bbox2d.top * s_camParams.height;
        entity.bbox2dUnprocessed.right = bbox2d.right * s_camParams.width;
        entity.bbox2dUnprocessed.bottom = bbox2d.bottom * s_camParams.height;

        //Calculated during processSegmentation and processOcclusion
        entity.pointsHit2D = 0;
        entity.pointsHit3D = 0;
        entity.occlusion = 0;

        entity.truncation = truncation;
        entity.pitch = pitch;
        entity.roll = roll;
        entity.model = model;
        //entity.modelString = modelString;
        entity.modelString = VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(model);
        entity.objType = type;

        if (trackFirstFrame.find(entityID) == trackFirstFrame.end()) {
            trackFirstFrame.insert(std::pair<int, int>(entityID, instance_index));
        }
        entity.trackFirstFrame = trackFirstFrame[entityID];

        entity.isPedInV = isPedInV;
        entity.vPedIsIn = vPedIsIn;

        entity.xVector = xVector;
        entity.yVector = yVector;
        entity.zVector = zVector;

        entity.SpeedVectorWC = speedVector;

        entity.object_vel_vector = convertCoordinateSystem(speedVector, m_forwardVector, m_rightVector, m_upVector);
        //entity.object_vel_vector = speedVector;
        Vector3 own_vehicle_velocity= ENTITY::GET_ENTITY_SPEED_VECTOR(m_ownVehicle, false);
        entity.ownvehicle_vel_vector = convertCoordinateSystem(own_vehicle_velocity, m_forwardVector, m_rightVector, m_upVector);
        log("End of getEntityVector");
    }

    return success;
}

void ObjectDetection::setVehiclesList() {
    m_curFrame.vehicles.clear();
    log("Setting vehicles list.");
    const int ARR_SIZE = 1024;
    Vehicle vehicles[ARR_SIZE];

    Hash model;
    int classid;

    int count = worldGetAllVehicles(vehicles, ARR_SIZE);
    for (int i = 0; i < count; i++) {
        if (vehicles[i] == m_vehicle) continue; //Don't process perspective car!

        model = ENTITY::GET_ENTITY_MODEL(vehicles[i]);
        if (VEHICLE::IS_THIS_MODEL_A_CAR(model)) classid = 0;
        else if (VEHICLE::IS_THIS_MODEL_A_BIKE(model)) classid = 1;
        else if (VEHICLE::IS_THIS_MODEL_A_BICYCLE(model)) classid = 2;
        else if (VEHICLE::IS_THIS_MODEL_A_QUADBIKE(model)) classid = 3;
        else if (VEHICLE::IS_THIS_MODEL_A_BOAT(model)) classid = 4;
        else if (VEHICLE::IS_THIS_MODEL_A_PLANE(model)) classid = 5;
        else if (VEHICLE::IS_THIS_MODEL_A_HELI(model)) classid = 6;
        else if (VEHICLE::IS_THIS_MODEL_A_TRAIN(model)) classid = 7;
        else if (VEHICLE::_IS_THIS_MODEL_A_SUBMERSIBLE(model)) classid = 8;
        else classid = 9; //unknown (ufo?)

        //Get the model string, convert it to lowercase then find it in lookup table
        std::string modelString = VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(model);
        std::string before = modelString;
        std::transform(modelString.begin(), modelString.end(), modelString.begin(), ::tolower);
        std::string type = "Unknown";
        modelString.erase(remove_if(modelString.begin(), modelString.end(), [](char c) { return !isalpha(c); }), modelString.end());
        auto search = m_vLookup.find(modelString);
        if (search != m_vLookup.end()) {
            type = search->second;
        }
        else {
            std::ostringstream oss;
            oss << "Entity Model/type/hash: " << modelString << ", " << type << ", " << model << ", before: " << before << ", index: " << instance_index;
            std::string str = oss.str();
            log(str, true);
        }

        if (type == "Unknown") {
            if (VEHICLE::IS_THIS_MODEL_A_CAR(model)) type = "UNK";
            else if (VEHICLE::IS_THIS_MODEL_A_BIKE(model)) type = "UNK";
            else if (VEHICLE::IS_THIS_MODEL_A_BICYCLE(model)) type = "UNK";
            else if (VEHICLE::IS_THIS_MODEL_A_QUADBIKE(model)) type = "UNK";
            else if (VEHICLE::IS_THIS_MODEL_A_BOAT(model)) type = "UNK";
            else if (VEHICLE::IS_THIS_MODEL_A_PLANE(model)) type = "UNK";
            else if (VEHICLE::IS_THIS_MODEL_A_HELI(model)) type = "UNK";
            else if (VEHICLE::IS_THIS_MODEL_A_TRAIN(model)) type = "UNK";
            else if (VEHICLE::_IS_THIS_MODEL_A_SUBMERSIBLE(model)) type = "UNK";
        }

        ObjEntity objEntity;
        //Gets set to false in getEntityVector if outside SECONDARY_PERSPECTIVE_RANGE or if vehicle is unoccupied
        bool nearbyVehicle = true;
        bool success = getEntityVector(objEntity, vehicles[i], model, classid, type, modelString, false, -1, nearbyVehicle);



        if (success) {
            if (m_curFrame.vehicles.find(objEntity.entityID) == m_curFrame.vehicles.end()) {
                m_curFrame.vehicles.insert(std::pair<int, ObjEntity>(objEntity.entityID, objEntity));
            }
        }
        //Only change m_nearbyVehicles for the ego vehicle
        if (nearbyVehicle && m_vehicle == m_ownVehicle) {
            m_nearbyVehicles.push_back(objEntity);
        }
    }
}

void ObjectDetection::setPedsList() {
    m_curFrame.peds.clear();
    log("Setting peds list.");
    const int ARR_SIZE = 1024;
    Ped peds[ARR_SIZE];

    Hash model;
    int classid;

    int count = worldGetAllPeds(peds, ARR_SIZE);
    for (int i = 0; i < count; i++) {
        bool isPedInV = false;
        Vehicle vPedIsIn = -1;
        if (PED::IS_PED_IN_ANY_VEHICLE(peds[i], TRUE)) {
            vPedIsIn = PED::GET_VEHICLE_PED_IS_IN(peds[i], FALSE);
            Hash vModel = ENTITY::GET_ENTITY_MODEL(vPedIsIn);

            //Update bounding boxes/stencils for all bike type vehicles
            if (VEHICLE::IS_THIS_MODEL_A_BIKE(vModel) ||
                VEHICLE::IS_THIS_MODEL_A_BICYCLE(vModel) ||
                VEHICLE::IS_THIS_MODEL_A_QUADBIKE(vModel)) {

                if (m_pedsInVehicles.find(vPedIsIn) != m_pedsInVehicles.end()) {
                    log("Putting ped in a vehicle in list.", true);
                    m_pedsInVehicles[vPedIsIn].push_back(peds[i]);
                }
                else {
                    m_pedsInVehicles.insert(std::pair<Vehicle, std::vector<Ped>>(vPedIsIn, { peds[i] }));
                }
            }
            isPedInV = true; //Don't add peds in vehicles as unique objects!
        }

        std::string type = "Pedestrian";
        if (PED::GET_PED_TYPE(peds[i]) == 28) {
            classid = 11; //animal
            type = "Misc";
        }
        else classid = PEDESTRIAN_CLASS_ID;

        if (RETAIN_ANIMALS || classid != 11) {
            model = ENTITY::GET_ENTITY_MODEL(peds[i]);

            ObjEntity objEntity;
            bool nearbyVehicle = false;//Don't look if it is a nearby vehicle as we're collecting peds
            bool success = getEntityVector(objEntity, peds[i], model, classid, type, type, isPedInV, vPedIsIn, nearbyVehicle);
            if (success) {
                if (m_curFrame.peds.find(objEntity.entityID) == m_curFrame.peds.end()) {
                    m_curFrame.peds.insert(std::pair<int, ObjEntity>(objEntity.entityID, objEntity));
                }
            }
        }
    }
}

void ObjectDetection::setFilenames() {
    //These are standard files
    m_imgFilename = getStandardFilename("image_2", ".png");
    m_veloFilename = getStandardFilename("velodyne", ".bin");
    m_depthFilename = getStandardFilename("depth", ".bin");
    m_stencilFilename = getStandardFilename("stencil", ".raw");
    m_labelsFilename = getStandardFilename("label_2", ".txt");
    m_labelsAugFilename = getStandardFilename("label_aug_2", ".txt");
    m_calibFilename = getStandardFilename("calib", ".txt");

    //Use case files
    m_velo_idealFileName = getStandardFilename("velodyne_ideal", ".bin");
    m_velo_zeroFileName = getStandardFilename("velodyne_zero", ".bin");
    m_velo_velocityFileName = getStandardFilename("velodyne_velocity", ".bin");


    //TODO - Why are two seg images being printed (there are some minor differences in images it appears)
    m_segImgFilename = getStandardFilename("segImage", ".png");
    m_instSegFilename = getStandardFilename("instSeg", ".png");
    m_instSegImgFilename = getStandardFilename("instSegImage", ".png");

    //These files are for relating object positions to each other
    if (GENERATE_SECONDARY_PERSPECTIVES) {
        m_posFilename = getStandardFilename("position_world", ".txt");
        m_egoObjectFilename = getStandardFilename("ego_object", ".txt");
    }

    //The following are for testing, only create filenames/directories if they are set to be on
    if (OUTPUT_DM_POINTCLOUD) {
        m_depthPCFilename = getStandardFilename("depthPC", ".bin");
        m_depthImgFilename = getStandardFilename("depthImage", ".png");
    }
    if (OUTPUT_OFFSET_POINTCLOUDS) {
        m_veloFilenameU = getStandardFilename("velodyneU", ".bin");
        m_depthPCFilenameU = getStandardFilename("depthPCU", ".bin");
    }
    if (OUTPUT_GROUND_PIXELS) {
        m_groundPointsFilename = getStandardFilename("groundPointsImg", ".png");
    }
    if (OUTPUT_UNUSED_PIXELS_IMAGE) m_unusedPixelsFilename = getStandardFilename("unusedPixelsImage", ".png");
    if (OUTPUT_STENCIL_IMAGE) m_stencilImgFilename = getStandardFilename("stencilImage", ".png");
    if (OUTPUT_OCCLUSION_IMAGE) m_occImgFilename = getStandardFilename("occlusionImage", ".png");
    if (OUTPUT_UNPROCESSED_LABELS) m_labelsUnprocessedFilename = getStandardFilename("labelsUnprocessed", ".txt");
}

void ObjectDetection::setupLiDAR() {
    if (pointclouds && !lidar_initialized) //flag if activate the LiDAR
    {
        //Specs on Velodyne HDL-64E
        //0.09f azimuth resolution
        //26.8 vertical fov (+2 degrees up to -24.8 degrees down)
        //0.420 vertical resolution
        lidar.Init3DLiDAR_FOV(MAX_LIDAR_DIST, 90.0f, 0.09f, 26.9f, 0.420f, 2.0f);
        lidar.AttachLiDAR2Camera(camera, ped);
        lidar_initialized = true;
        m_pDMPointClouds = (float *)malloc(s_camParams.width * s_camParams.height * FLOATS_PER_POINT * sizeof(float));
        m_pDMImage = (uint16_t *)malloc(s_camParams.width * s_camParams.height * sizeof(uint16_t));
        m_pStencilImage = (uint8_t *)malloc(s_camParams.width * s_camParams.height * sizeof(uint8_t));
        m_pOcclusionImage = (uint8_t *)malloc(s_camParams.width * s_camParams.height * sizeof(uint8_t));
        m_pUnusedStencilImage = (uint8_t *)malloc(s_camParams.width * s_camParams.height * sizeof(uint8_t));
        
        //RGB Image needs 3 bytes per value
        m_stencilSegLength = s_camParams.width * s_camParams.height * 3 * sizeof(uint8_t);
        m_instanceSegLength = s_camParams.width * s_camParams.height * sizeof(uint32_t);
        m_instanceSegImgLength = s_camParams.width * s_camParams.height * 3 * sizeof(uint8_t);

        m_pStencilSeg = (uint8_t *)malloc(m_stencilSegLength);
        m_pInstanceSeg = (uint32_t *)malloc(m_instanceSegLength);
        m_pInstanceSegImg = (uint8_t *)malloc(m_instanceSegImgLength);
        m_pGroundPointsImage = (uint8_t *)malloc(s_camParams.width * s_camParams.height * FLOATS_PER_POINT * sizeof(uint8_t));
    }
}

void ObjectDetection::collectLiDAR() {
    m_entitiesHit.clear();
    lidar.updateCurrentPosition(m_camForwardVector, m_camRightVector, m_camUpVector);
    float *pointCloud = lidar.GetPointClouds(pointCloudSize, &m_entitiesHit, lidar_param, m_pDepth, m_pInstanceSeg, m_vehicle);

    // Output point clouds dimensions
    uint OUTPUT_POINTCLOUD_POINTS = 4;

    // Create new arrays with the points as pointCloud will have all the offsets.
    float* IdealArray = new float[pointCloudSize * sizeof(float) * OUTPUT_POINTCLOUD_POINTS];
    float* EntityArray = new float[pointCloudSize * sizeof(float) * OUTPUT_POINTCLOUD_POINTS];
    float* ZeroIntensityArray = new float[pointCloudSize * sizeof(float) * OUTPUT_POINTCLOUD_POINTS];
    float* VelocityArray = new float[pointCloudSize * sizeof(float) * OUTPUT_POINTCLOUD_POINTS];

    int i = 0, k=0;
    while( i < (pointCloudSize * sizeof(float) * OUTPUT_POINTCLOUD_POINTS)) {

        // No use case: intensity is the entity ID of each object
        // *(p + 3)
        *(EntityArray + i) = pointCloud[k];
        *(EntityArray + i + 1) = pointCloud[k + 1];
        *(EntityArray + i + 2) = pointCloud[k + 2];
        *(EntityArray + i + 3) = pointCloud[k + 3];


        // Use case: Ideal dataset -> 'Car' labeled objects = 1, others = '0'
        // *(p + 4)
        *(IdealArray + i) = pointCloud[k];
        *(IdealArray + i + 1) = pointCloud[k + 1];
        *(IdealArray + i + 2) = pointCloud[k + 2];
        *(IdealArray + i + 3) = pointCloud[k + 4];

        // Use case: Zero intensity dataset -> all points have 0 intensity
        // *(p + 5)
        *(ZeroIntensityArray + i) = pointCloud[k];
        *(ZeroIntensityArray + i + 1) = pointCloud[k + 1];
        *(ZeroIntensityArray + i + 2) = pointCloud[k + 2];
        *(ZeroIntensityArray + i + 3) = pointCloud[k + 5];

        // Use case: Zero intensity dataset -> all points have 0 intensity
        // *(p + 5)
        *(ZeroIntensityArray + i) = pointCloud[k];
        *(ZeroIntensityArray + i + 1) = pointCloud[k + 1];
        *(ZeroIntensityArray + i + 2) = pointCloud[k + 2];
        *(ZeroIntensityArray + i + 3) = pointCloud[k + 5];

        // Use case: Zero intensity dataset -> all points have 0 intensity
        // *(p + 5)
        *(VelocityArray + i) = pointCloud[k];
        *(VelocityArray + i + 1) = pointCloud[k + 1];
        *(VelocityArray + i + 2) = pointCloud[k + 2];
        *(VelocityArray + i + 3) = pointCloud[k + 6];


        i += 4; //Update according to output pointclouds dimensions
        k += 7; //Update according to  lidar.GetPointClouds() dimensions (FLOATS_PER_POINT)
    }

    /* -------------------------- OUTPUT FILES -------------------------------------------------------*/

    // No use case: entity ID as intensity
    // Folder: velodyne
    std::ofstream ofile(m_veloFilename, std::ios::binary);
    ofile.write((char*)EntityArray, OUTPUT_POINTCLOUD_POINTS * sizeof(float) * pointCloudSize);
    ofile.close();

    // USE CASE: Ideal dataset -> Car = 1, others = 0
    // Folder: velodyne_ideal
    std::ofstream ofileideal(m_velo_idealFileName, std::ios::binary);
    ofileideal.write((char*)IdealArray, OUTPUT_POINTCLOUD_POINTS * sizeof(float) * pointCloudSize);
    ofileideal.close();

    // USE CASE: Zero intensity dataset -> all = 0
    // Folder: velodyne_zero
    std::ofstream ofilezero(m_velo_zeroFileName, std::ios::binary);
    ofilezero.write((char*)ZeroIntensityArray, OUTPUT_POINTCLOUD_POINTS * sizeof(float) * pointCloudSize);
    ofilezero.close();


    // USE CASE: Velocity dataset -> relative velocity on each point
    // Folder: velodyne_velocity
    std::ofstream ofilevel(m_velo_velocityFileName, std::ios::binary);
    ofilevel.write((char*)VelocityArray, OUTPUT_POINTCLOUD_POINTS * sizeof(float) * pointCloudSize);
    ofilevel.close();



    if (OUTPUT_RAYCAST_POINTS) {
        int pointCloudSize2;
        float* pointCloud2 = lidar.GetRaycastPointcloud(pointCloudSize2);

        std::string filename2 = getStandardFilename("velodyneRaycast", ".bin");
        std::ofstream ofile2(filename2, std::ios::binary);
        ofile2.write((char*)pointCloud2, FLOATS_PER_POINT * sizeof(float)*pointCloudSize2);
        ofile2.close();
    }

    if (GENERATE_2D_POINTMAP) {
        //Used for obtaining the 2D points for sampling depth map to convert to velodyne pointcloud
        int size;
        float * points2D = lidar.Get2DPoints(size);

        std::string filename = getStandardFilename("2dpoints", ".bin");
        std::ofstream ofile2(filename, std::ios::binary);
        ofile2.write((char*)points2D, 2 * sizeof(float) * size);
        ofile2.close();

        //Prints out the real values for a sample of the 
        filename = getStandardFilename("2dpoints", ".txt");
        FILE* f = fopen(filename.c_str(), "w");
        fclose(f);
        f = fopen(filename.c_str(), "a");
        int i = 0;
        std::ostringstream oss;
        while (i < 100) {
            oss << "num: " << i << " x: " << points2D[2 * i] << " y: " << points2D[2 * i + 1] << "\n";
            ++i;
        }
        i = (size / 2);
        int maxPrint = i + 100;
        while (i < maxPrint) {
            oss << "num: " << i << " x: " << points2D[2 * i] << " y: " << points2D[2 * i + 1] << "\n";
            ++i;
        }
        i = size - 100;
        while (i < size) {
            oss << "num: " << i << " x: " << points2D[2 * i] << " y: " << points2D[2 * i + 1] << "\n";
            ++i;
        }
        std::string str = oss.str();
        fprintf(f, str.c_str());
        fclose(f);
    }

    if (OUTPUT_DEPTH_STATS) {
        lidar.printDepthStats();
    }
}

void ObjectDetection::setStencilBuffer() {
    log("About to set stencil buffer");
    int size = s_camParams.width * s_camParams.height;

    std::ofstream ofile(m_stencilFilename, std::ios::binary);
    ofile.write((char*)m_pStencil, size);
    ofile.close();

    std::vector<int> stencilValues;
    log("After writing stencil buffer");
    for (int j = 0; j < s_camParams.height; ++j) {
        for (int i = 0; i < s_camParams.width; ++i) {
            uint8_t val = m_pStencil[j * s_camParams.width + i];
            uint8_t* p = m_pStencilImage + (j * s_camParams.width) + i;

            if (val == 2) {
                *p = 255; //Vehicles
            }
            else if (val == 1) {
                *p = 128; //NPCs
            }
            else {
                *p = val;
            }

            if (OUTPUT_SEPARATE_STENCILS) {
                bool newValue = true;
                if (ONLY_OUTPUT_UNKNOWN_STENCILS && std::find(KNOWN_STENCIL_TYPES.begin(), KNOWN_STENCIL_TYPES.end(), val) != KNOWN_STENCIL_TYPES.end()) {
                    newValue = false;
                }
                if (std::find(stencilValues.begin(), stencilValues.end(), val) != stencilValues.end()) {
                    newValue = false;
                }
                if (newValue) {
                    stencilValues.push_back(val);
                }
            }
        }
    }

    if (OUTPUT_STENCIL_IMAGE) {
        log("Before saving stencil image");
        std::vector<std::uint8_t> ImageBuffer;
        lodepng::encode(ImageBuffer, (unsigned char*)m_pStencilImage, s_camParams.width, s_camParams.height, LCT_GREY, 8);
        lodepng::save_file(ImageBuffer, m_stencilImgFilename);
        log("After saving stencil image");
    }

    if (OUTPUT_SEPARATE_STENCILS) {
        for (int s : stencilValues) {
            int count = 0;
            for (int j = 0; j < s_camParams.height; ++j) {
                for (int i = 0; i < s_camParams.width; ++i) {
                    uint8_t val = m_pStencil[j * s_camParams.width + i];
                    uint8_t* p = m_pStencilImage + (j * s_camParams.width) + i;
                    if (val == s) {
                        *p = 255; //Stencil value
                        ++count;
                    }
                    else {
                        *p = 0;
                    }
                }
            }

            std::string filename = baseFolder + "stencilImage" + "\\";
            CreateDirectory(filename.c_str(), NULL);
            filename.append("\\");
            filename.append(instance_string);
            filename.append("-");
            filename.append(std::to_string(s));
            filename.append(".png");
            std::vector<std::uint8_t> ImageBuffer;
            lodepng::encode(ImageBuffer, (unsigned char*)m_pStencilImage, s_camParams.width, s_camParams.height, LCT_GREY, 8);
            lodepng::save_file(ImageBuffer, filename);

            if (ONLY_OUTPUT_UNKNOWN_STENCILS) {
                std::ostringstream oss;
                oss << "***************************************unknown stencil type: " << s << " at index: " << instance_index << " with count: " << count;
                log(oss.str(), true);
            }
        }
    }

    if (ONLY_OUTPUT_UNKNOWN_STENCILS) {
        std::ostringstream oss;
        oss << "Stencil types: ";
        for (int s : KNOWN_STENCIL_TYPES) {
            oss << s << ", ";
        }
        log(oss.str(), true);
    }
}

void ObjectDetection::setDepthBuffer(bool prevDepth) {
    int size = s_camParams.width * s_camParams.height;
    log("About to set depth buffer");

    std::string depthPCFilename = m_depthPCFilename;
    if (prevDepth) {
        float * pointCloud = lidar.UpdatePointCloud(pointCloudSize, m_pDepth);
        std::ofstream ofile1(m_veloFilenameU, std::ios::binary);
        ofile1.write((char*)pointCloud, FLOATS_PER_POINT * sizeof(float) * pointCloudSize);
        ofile1.close();
        depthPCFilename = m_depthPCFilenameU;
    }

    std::ofstream ofile(m_depthFilename, std::ios::binary);
    ofile.write((char*)m_pDepth, size * sizeof(float));
    ofile.close();

    int nonzero = 0;
    if (OUTPUT_DM_POINTCLOUD || OUTPUT_GROUND_PIXELS) {
        int pointCount = 0;
        float maxDepth = 0;
        float minDepth = 1;
        for (int j = 0; j < s_camParams.height; ++j) {
            for (int i = 0; i < s_camParams.width; ++i) {
                float ndc = m_pDepth[j * s_camParams.width + i];
                if (ndc != 0) {
                    ++nonzero;
                }
                Vector3 relPos = depthToCamCoords(ndc, i, j);

                if (OUTPUT_GROUND_PIXELS) {
                    int s = m_pStencil[j * s_camParams.width + i];
                    bool groundPoint;
                    if (s == STENCIL_TYPE_SKY || s == STENCIL_TYPE_NPC || s == STENCIL_TYPE_OWNCAR ||
                        s == STENCIL_TYPE_VEGETATION || s == STENCIL_TYPE_VEHICLE || s == STENCIL_TYPE_SELF) {
                        groundPoint = false;
                    }
                    else {
                        Vector3 worldPos = camToWorld(relPos, m_camForwardVector, m_camRightVector, m_camUpVector);
                        float groundZ;
                        //Note should always do +2 to ensure it hits the proper ground point
                        GAMEPLAY::GET_GROUND_Z_FOR_3D_COORD(worldPos.x, worldPos.y, worldPos.z + 2, &(groundZ), 0);
                        groundPoint = (worldPos.z - groundZ) < GROUND_POINT_MAX_DIST;
                    }
                    uint8_t pointVal = groundPoint ? 255 : 0;
                    m_pGroundPointsImage[j * s_camParams.width + i] = pointVal;
                }

                if (OUTPUT_DM_POINTCLOUD) {
                    float distance = sqrt(SYSTEM::VDIST2(0, 0, 0, relPos.x, relPos.y, relPos.z));
                    if ((OUTPUT_FULL_DM_POINTCLOUD || distance <= MAX_LIDAR_DIST) && distance >= s_camParams.nearClip) {
                        float* p = m_pDMPointClouds + (pointCount * 4);
                        *p = relPos.y;
                        *(p + 1) = -relPos.x;
                        *(p + 2) = relPos.z;
                        *(p + 3) = 0;
                        pointCount++;
                    }

                    uint16_t* p = m_pDMImage + (j * s_camParams.width) + i;
                    float distClipped = 1 - std::min<float>(1.0f, (distance - s_camParams.nearClip) / (s_camParams.farClip - s_camParams.nearClip));
                    uint16_t num = (uint16_t)floor(distClipped * 65535);
                    uint16_t swapped = (num >> 8) | (num << 8);
                    *p = swapped;
                }
            }
        }

        if (OUTPUT_GROUND_PIXELS) {
            std::vector<std::uint8_t> ImageBuffer;
            lodepng::encode(ImageBuffer, (unsigned char*)m_pGroundPointsImage, s_camParams.width, s_camParams.height, LCT_GREY, 8);
            lodepng::save_file(ImageBuffer, m_groundPointsFilename);
        }
        
        if (OUTPUT_DM_POINTCLOUD) {
            std::ostringstream oss;
            oss << "Min depth: " << minDepth << " max: " << maxDepth << " pointCount: " << pointCount <<
                " height: " << s_camParams.height << " width: " << s_camParams.width << " size: " << size <<
                "\n Nonzero depth values: " << nonzero;
            std::string str = oss.str();
            log(str);

            std::ofstream ofile1(depthPCFilename, std::ios::binary);
            ofile1.write((char*)m_pDMPointClouds, FLOATS_PER_POINT * sizeof(float) * pointCount);
            ofile1.close();

            std::vector<std::uint8_t> ImageBuffer;
            lodepng::encode(ImageBuffer, (unsigned char*)m_pDMImage, s_camParams.width, s_camParams.height, LCT_GREY, 16);
            lodepng::save_file(ImageBuffer, m_depthImgFilename);

            log("After saving DM pointcloud");
        }
    }
}

//ndc is Normalized Device Coordinates which is value received from depth buffer
Vector3 ObjectDetection::depthToCamCoords(float ndc, float screenX, float screenY) {
    float normScreenX = 2 * screenX / float(s_camParams.width - 1) - 1.0f;
    float normScreenY = 2 * screenY / float(s_camParams.height - 1) - 1.0f;

    float ncX = normScreenX * s_camParams.ncWidth / 2;
    float ncY = normScreenY * s_camParams.ncHeight / 2;

    //Distance to near clip (hypotenus)
    float d2nc = sqrt(s_camParams.nearClip * s_camParams.nearClip + ncX * ncX + ncY * ncY);
    float depth = d2nc / ndc;
    if (ndc <= 0 || depth > s_camParams.farClip) {
        depth = s_camParams.farClip;
    }

    float depthDivisor = (s_camParams.nearClip * depth) / (2 * s_camParams.farClip);
    depth = depth / (1 + depthDivisor);

    //X is right, Y is forward, Z is up (GTA coordinate frame)
    Vector3 unitVec;
    unitVec.x = ncX / d2nc;
    unitVec.y = s_camParams.nearClip / d2nc;
    unitVec.z = -ncY / d2nc;

    Vector3 relPos;
    relPos.x = unitVec.x * depth;
    relPos.y = unitVec.y * depth;
    relPos.z = unitVec.z * depth;

    return relPos;
}

void ObjectDetection::increaseIndex() {
    if (pointclouds) {
        if (lidar_initialized) {
            ++instance_index;
        }
    }
    else {
        ++instance_index;
    }
    if (collectTracking && instance_index == trSeriesLength) {
        instance_index = 0;
        ++series_index;
        trSeriesGap = true;
        trackFirstFrame.clear();

        //update the string
        char temp[] = "%04d";
        char strComp[sizeof temp + 100];
        sprintf(strComp, temp, series_index);
        series_string = strComp;
    }

    char temp[] = "%06d";
    char strComp[sizeof temp + 100];
    sprintf(strComp, temp, instance_index);
    instance_string = strComp;
}

void ObjectDetection::setIndex() {
    m_curFrame.instanceIdx = instance_index;
    m_curFrame.seriesIdx = series_index;
}

//Camera intrinsics are focal length, and center in horizontal (x) and vertical (y)
void ObjectDetection::calcCameraIntrinsics() {
    float f = s_camParams.width / (2 * tan(HOR_CAM_FOV * PI / 360));
    float cx = s_camParams.width / 2;
    float cy = s_camParams.height / 2;

    intrinsics[0] = f;
    intrinsics[1] = cx;
    intrinsics[2] = cy;

    if (DEBUG_LOGGING) {
        std::ostringstream oss;
        oss << "Focal length is: " << f;
        std::string str = oss.str();
        log(str);
    }

    m_curFrame.focalLen = f;
}

//Camera intrinsics are focal length, and center in horizontal (x) and vertical (y)
void ObjectDetection::setFocalLength() {
    m_curFrame.focalLen = intrinsics[0];
}

std::string ObjectDetection::getStandardFilename(std::string subDir, std::string extension) {
    std::string filename = baseFolder;
    CreateDirectory(filename.c_str(), NULL);

    if (m_vPerspective != -1) {
        char temp[] = "%07d";
        char strComp[sizeof temp + 100];
        sprintf(strComp, temp, m_vPerspective);
        std::string entityStr = strComp;

        filename.append("alt_perspective");
        filename.append("\\");
        CreateDirectory(filename.c_str(), NULL);
        filename.append(entityStr);
        filename.append("\\");
        CreateDirectory(filename.c_str(), NULL);
    }

    filename.append(subDir);
    filename.append("\\");
    CreateDirectory(filename.c_str(), NULL);
    if (collectTracking) {
        filename.append(series_string);
        CreateDirectory(filename.c_str(), NULL);
    }
    filename.append("\\");
    filename.append(instance_string);
    filename.append(extension);
    return filename;
}

void ObjectDetection::outputRealSpeed() {
    //Print to file after every complete series
    if (m_trackLastSeqIndex != series_index) {
        m_trackDistErrorTotal /= m_trackDistErrorTotalCount;
        m_trackDistErrorTotalVar /= m_trackDistErrorTotalCount;
        m_trackDistErrorTotalVar = sqrt(m_trackDistErrorTotalVar);
        float avgSpeed = m_trackRealSpeed / m_trackDistErrorTotalCount;
        float avgDist = m_trackDist / m_trackDistErrorTotalCount;

        FILE* f = fopen(m_timeTrackFile.c_str(), "a");
        std::ostringstream oss;
        oss << m_trackDistErrorTotal << " " << m_trackDistErrorTotalVar << " " << avgSpeed << " " << avgDist;
        std::string str = oss.str();
        fprintf(f, str.c_str());
        fprintf(f, "\n");
        fclose(f);

        //Reset for every sequence
        m_trackRealSpeed = 0;
        m_trackDist = 0;
        m_trackDistErrorTotal = 0;
        m_trackDistErrorTotalVar = 0;
        m_trackDistErrorTotalCount = 0;
        m_trackLastSeqIndex = series_index;
    }

    //Initialize for every sequence (after printing)
    if (instance_index == 0) {
        m_trackLastPos = s_camParams.pos;
        m_trackLastIndex = instance_index;
        m_trackLastRealSpeed = ENTITY::GET_ENTITY_SPEED(m_vehicle) / 10;
        return;
    }

    //Do not count if we are in the gap between sequences
    if (m_trackLastIndex == instance_index) {
        return;
    }

    //Intermediate results
    if (instance_index % 10 == 0) {
        if (instance_index != 0) {
            std::ostringstream oss;
            float avgSpeed = m_trackRealSpeed * 10 / m_trackDistErrorTotalCount;
            float avgDist = m_trackDist * 10 / m_trackDistErrorTotalCount;
            oss << "Speed: " << avgSpeed << " dist: " << avgDist;
            std::string str = oss.str();
            log(str);
        }
    }

    //Update values
    //Average of speed at last frame and current frame
    m_trackRealSpeed += (ENTITY::GET_ENTITY_SPEED(m_vehicle) / 10 + m_trackLastRealSpeed) / 2;
    m_trackDist += sqrt(SYSTEM::VDIST2(s_camParams.pos.x, s_camParams.pos.y, s_camParams.pos.z, m_trackLastPos.x, m_trackLastPos.y, m_trackLastPos.z));
    m_trackDistErrorTotal += m_trackDist - m_trackRealSpeed;
    m_trackDistErrorTotalVar += pow((m_trackDist - m_trackRealSpeed), 2);
    m_trackDistErrorTotalCount++;

    m_trackLastPos = s_camParams.pos;
    m_trackLastIndex = instance_index;
    m_trackLastRealSpeed = ENTITY::GET_ENTITY_SPEED(m_vehicle) / 10;
}

void ObjectDetection::setCamParams(float* forwardVec, float* rightVec, float* upVec) {
    //These values stay the same throughout a collection period
    if (!s_camParams.init) {
        s_camParams.nearClip = 0.15;// CAM::_0xD0082607100D7193(); //CAM::GET_CAM_NEAR_CLIP(camera);
        s_camParams.farClip = 10001.5;// 800;// CAM::_0xDFC8CBC606FDB0FC(); //CAM::GET_CAM_FAR_CLIP(camera);
        s_camParams.fov = 59;// CAM::GET_GAMEPLAY_CAM_FOV();//CAM::GET_CAM_FOV(camera);
        s_camParams.ncHeight = 2 * s_camParams.nearClip * tan(s_camParams.fov / 2. * (PI / 180.)); // field of view is returned vertically
        s_camParams.ncWidth = s_camParams.ncHeight * GRAPHICS::_GET_SCREEN_ASPECT_RATIO(false);
        s_camParams.init = true;
    }

    ENTITY::GET_ENTITY_MATRIX(m_vehicle, &m_camForwardVector, &m_camRightVector, &m_camUpVector, &s_camParams.pos);

    if (forwardVec) {
        m_camForwardVector.x = forwardVec[0];
        m_camForwardVector.y = forwardVec[1];
        m_camForwardVector.z = forwardVec[2];

        if (rightVec && upVec) {
            m_camRightVector.x = rightVec[0];
            m_camRightVector.y = rightVec[1];
            m_camRightVector.z = rightVec[2];
            m_camUpVector.x = upVec[0];
            m_camUpVector.y = upVec[1];
            m_camUpVector.z = upVec[2];
        }
    }
    else {
        ENTITY::GET_ENTITY_MATRIX(m_vehicle, &m_camForwardVector, &m_camRightVector, &m_camUpVector, &currentPos);
    }

    //These values change frame to frame
    //Camera functions do not work in eve. Need to use vehicle and offsets.
    //Recordings need to always have the camera aligned with the vehicle for export to be aligned properly.
    if (!m_eve) {
        s_camParams.theta = ENTITY::GET_ENTITY_ROTATION(m_vehicle, 0); //CAM::GET_GAMEPLAY_CAM_ROT(0); //CAM::GET_CAM_ROT(camera, 0);
    }
    //s_camParams.pos = currentPos;// CAM::GET_GAMEPLAY_CAM_COORD();// CAM::GET_CAM_COORD(camera);
    //Use vehicleForwardVector since it corresponds to vehicle forwardVector
    s_camParams.pos.x = s_camParams.pos.x + CAM_OFFSET_FORWARD * vehicleForwardVector.x + CAM_OFFSET_UP * vehicleUpVector.x;
    s_camParams.pos.y = s_camParams.pos.y + CAM_OFFSET_FORWARD * vehicleForwardVector.y + CAM_OFFSET_UP * vehicleUpVector.y;
    s_camParams.pos.z = s_camParams.pos.z + CAM_OFFSET_FORWARD * vehicleForwardVector.z + CAM_OFFSET_UP * vehicleUpVector.z;

    Vector3 theta = CAM::GET_CAM_ROT(camera, 0);
    Vector3 pos1 = CAM::GET_CAM_COORD(camera);
    Vector3 rotation = ENTITY::GET_ENTITY_ROTATION(m_vehicle, 0);

    //For optimizing 3d to 2d and unit vector to 2d calculations
    s_camParams.eigenPos = Eigen::Vector3f(s_camParams.pos.x, s_camParams.pos.y, s_camParams.pos.z);
    s_camParams.eigenRot = Eigen::Vector3f(s_camParams.theta.x, s_camParams.theta.y, s_camParams.theta.z);
    s_camParams.eigenTheta = (PI / 180.0) * s_camParams.eigenRot;
    s_camParams.eigenCamDir = rotate(WORLD_NORTH, s_camParams.eigenTheta);
    s_camParams.eigenCamUp = rotate(WORLD_UP, s_camParams.eigenTheta);
    s_camParams.eigenCamEast = rotate(WORLD_EAST, s_camParams.eigenTheta);
    s_camParams.eigenClipPlaneCenter = s_camParams.eigenPos + s_camParams.nearClip * s_camParams.eigenCamDir;
    s_camParams.eigenCameraCenter = -s_camParams.nearClip * s_camParams.eigenCamDir;

    std::ostringstream oss1;
    oss1 << "\ns_camParams.pos X: " << s_camParams.pos.x << " Y: " << s_camParams.pos.y << " Z: " << s_camParams.pos.z <<
        "\nvehicle.pos X: " << currentPos.x << " Y: " << currentPos.y << " Z: " << currentPos.z <<
        "\npos1 - rendering cam X: " << pos1.x << " Y: " << pos1.y << " Z: " << pos1.z <<
        "\nfar: " << s_camParams.farClip << " nearClip: " << s_camParams.nearClip << " fov: " << s_camParams.fov <<
        "\nrotation gameplay: " << s_camParams.theta.x << " Y: " << s_camParams.theta.y << " Z: " << s_camParams.theta.z <<
        "\nrotation rendering: " << theta.x << " Y: " << theta.y << " Z: " << theta.z <<
        "\nrotation vehicle: " << rotation.x << " Y: " << rotation.y << " Z: " << rotation.z <<
        "\n AspectRatio: " << GRAPHICS::_GET_SCREEN_ASPECT_RATIO(false) <<
        "\nforwardVector: " << vehicleForwardVector.x << " Y: " << vehicleForwardVector.y << " Z: " << vehicleForwardVector.z;
    std::string str1 = oss1.str();
    log(str1);
}

void ObjectDetection::printSegImage() {
    int notUsedStencilPoints = 0;

    for (int j = 0; j < s_camParams.height; ++j) {
        for (int i = 0; i < s_camParams.width; ++i) {
            uint8_t stencilVal = m_pStencil[j * s_camParams.width + i];
            if (stencilVal == STENCIL_TYPE_NPC || stencilVal == STENCIL_TYPE_VEHICLE) {
                if (m_pStencilSeg[3 * (j * s_camParams.width + i)] == 0 &&
                    m_pStencilSeg[3 * (j * s_camParams.width + i) + 1] == 0 &&
                    m_pStencilSeg[3 * (j * s_camParams.width + i) + 2] == 0) {
                    m_pUnusedStencilImage[j * s_camParams.width + i] = 255;
                    ++notUsedStencilPoints;
                }
            }
        }
    }
    if (notUsedStencilPoints > 10000) {
        std::ostringstream oss;
        oss << "***Lots of unused stencil points, total unused: " << notUsedStencilPoints << " instance idx: " << instance_index;
        if (collectTracking) {
            oss << " seq idx: " << series_index;
        }
        std::string str = oss.str();
        log(str, true);
    }
    FILE* f = fopen(m_usedPixelFile.c_str(), "a");
    std::ostringstream oss;
    oss << notUsedStencilPoints << " " << instance_index;
    if (collectTracking) {
        oss << " " << series_index;
    }
    std::string str = oss.str();
    fprintf(f, str.c_str());
    fprintf(f, "\n");
    fclose(f);

    std::vector<std::uint8_t> ImageBuffer;
    lodepng::encode(ImageBuffer, (unsigned char*)m_pStencilSeg, s_camParams.width, s_camParams.height, LCT_RGB, 8);
    lodepng::save_file(ImageBuffer, m_segImgFilename);

    //Print instance segmented image
    cv::Mat tempMat(cv::Size(s_camParams.width, s_camParams.height), CV_32SC1, m_pInstanceSeg);
    imwrite(m_instSegFilename, tempMat);
    tempMat.release();


    //Create and print out instance seg image in colour for visualization
    for (int j = 0; j < s_camParams.height; ++j) {
        for (int i = 0; i < s_camParams.width; ++i) {
            //RGB image is 3 bytes per pixel
            int idx = j * s_camParams.width + i;
            int segIdx = 3 * idx;
            int entityID = m_pInstanceSeg[idx];

            int newVal = 47 * entityID; //Just to produce unique but different colours
            int red = (newVal + 13 * entityID) % 255;
            int green = (newVal / 255) % 255;
            int blue = newVal % 255;
            uint8_t* p = m_pInstanceSegImg + segIdx;
            *p = red;
            *(p + 1) = green;
            *(p + 2) = blue;
        }
    }

    cv::Mat colorImg(cv::Size(s_camParams.width, s_camParams.height), CV_8UC3, m_pInstanceSegImg);
    log("About to print seg image3", true);
    imwrite(m_instSegImgFilename, colorImg);
    colorImg.release();

    //Clear all maps and seg image arrays
    memset(m_pStencilSeg, 0, m_stencilSegLength);
    memset(m_pInstanceSeg, 0, m_instanceSegLength);
    memset(m_pInstanceSegImg, 0, m_instanceSegImgLength);
}

void ObjectDetection::initVehicleLookup() {
    if (!m_vLookupInit) {
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
            m_vLookup.insert(std::pair< std::string, std::string>(model, vehicleType));

            //Modelstrings seem to be missing last few letters on occasion (this should fix that)
            if (model.size() >= 2) {
                model = model.substr(0, model.size() - 1);
                m_vLookup.insert(std::pair< std::string, std::string>(model, vehicleType));
            }
            if (model.size() >= 2) {
                model = model.substr(0, model.size() - 1);
                m_vLookup.insert(std::pair< std::string, std::string>(model, vehicleType));
            }
            if (model.size() >= 2) {
                model = model.substr(0, model.size() - 1);
                m_vLookup.insert(std::pair< std::string, std::string>(model, vehicleType));
            }
        }
        m_vLookupInit = true;
    }
}

void ObjectDetection::outputOcclusion() {
    if (OUTPUT_OCCLUSION_IMAGE) {
        std::vector<std::uint8_t> ImageBuffer;
        lodepng::encode(ImageBuffer, (unsigned char*)m_pOcclusionImage, s_camParams.width, s_camParams.height, LCT_GREY, 8);
        lodepng::save_file(ImageBuffer, m_occImgFilename);
        memset(m_pOcclusionImage, 0, s_camParams.width * s_camParams.height);
    }
}

void ObjectDetection::outputUnusedStencilPixels() {
    if (OUTPUT_UNUSED_PIXELS_IMAGE) {
        std::vector<std::uint8_t> ImageBuffer;
        lodepng::encode(ImageBuffer, (unsigned char*)m_pUnusedStencilImage, s_camParams.width, s_camParams.height, LCT_GREY, 8);
        lodepng::save_file(ImageBuffer, m_unusedPixelsFilename);
        memset(m_pUnusedStencilImage, 0, s_camParams.width * s_camParams.height);
    }
}

void ObjectDetection::exportEntity(ObjEntity e, std::ostringstream& oss, bool unprocessed, bool augmented,
                                    bool checkbbox2d, const int &maxDist, const int &min2DPoints, const int &min3DPoints) {
    //Skip peds in vehicles except for augmented labels (since they can be specified as peds in vehicles)
    if (e.isPedInV && !augmented) {
        return;
    }

    BBox2D b = e.bbox2d;
    if (unprocessed) b = e.bbox2dUnprocessed;

    if (checkbbox2d) {
        if ((int)b.left >= s_camParams.width || (int)b.right == 0 || (int)b.bottom == 0 || (int)b.top >= s_camParams.height) return;
        if ((int)b.left == (int)b.right || (int)b.top == (int)b.bottom) return;
    }

    if (maxDist != -1) {
        if (e.distance > float(maxDist)) return;

        //HAS_ENTITY_CLEAR_LOS_TO_ENTITY is from vehicle, NOT camera perspective
        //pointsHit misses some objects
        //hasLOSToEntity retrieves from camera perspective however 3D bboxes are larger than object
        if (ENTITY::IS_ENTITY_OCCLUDED(e.entityID) &&
            !ENTITY::HAS_ENTITY_CLEAR_LOS_TO_ENTITY(m_vehicle, e.entityID, 19) &&
            e.pointsHit3D <= 0) {
            log("Occluded and no points.", true);

            Vector3 upVector, rightVector, forwardVector, position; //Vehicle position
            ENTITY::GET_ENTITY_MATRIX(e.entityID, &forwardVector, &rightVector, &upVector, &position);
            if (!hasLOSToEntity(e.entityID, e.location, e.dim, forwardVector, rightVector, upVector)) {
                log("Occluded and no points2.", true);
                return;
            }
        }
    }
    if (min2DPoints != -1) {
        if (e.pointsHit2D < min2DPoints) {
            log("pointsHit2D failed.", true);
            return;
        }
    }

    oss << e.objType << " " << e.truncation << " " << e.occlusion << " " << e.alpha << " " <<
        (int)b.left << " " << (int)b.top << " " <<
        (int)b.right << " " << (int)b.bottom << " " <<
        e.height << " " << e.width << " " << e.length << " " <<
        e.location.x << " " << e.location.y << " " << e.location.z << " " <<
        e.rotation_y;

    if (augmented) {
        int vPedIsIn = e.isPedInV ? e.vPedIsIn : 0;
        oss << " " << e.entityID << " " << e.pointsHit2D << " " << e.pointsHit3D << " " << e.speed << " "
            << e.roll << " " << e.pitch << " " << e.modelString << " " << vPedIsIn << " " << e.object_vel_vector.x << " " << e.object_vel_vector.y << " " << e.object_vel_vector.z << " " << e.ownvehicle_vel_vector.x << " " << e.ownvehicle_vel_vector.y << " " << e.ownvehicle_vel_vector.z << " " << e.SpeedVectorWC.x << " " << e.SpeedVectorWC.y << " " << e.SpeedVectorWC.z;
    }
    oss << "\n";
}

void ObjectDetection::exportEntities(EntityMap entMap, std::ostringstream& oss, bool unprocessed, bool augmented, bool checkbbox2d, const int &maxDist, const int &min2DPoints, const int &min3DPoints) {
    for (EntityMap::const_iterator it = entMap.begin(); it != entMap.end(); ++it)
    {
        ObjEntity entity = it->second;
        exportEntity(entity, oss, unprocessed, augmented, checkbbox2d, maxDist, min2DPoints, min3DPoints);
    }
}

void ObjectDetection::exportPosition() {
    FILE* f = fopen(m_posFilename.c_str(), "w");
    std::ostringstream oss;

    oss << m_curFrame.kittiWorldPos.x << " " << m_curFrame.kittiWorldPos.y << " " << m_curFrame.kittiWorldPos.z << "\n"
        << m_curFrame.camPos.x << " " << m_curFrame.camPos.y << " " << m_curFrame.camPos.z << "\n"
        << m_curFrame.forwardVec.x << " " << m_curFrame.forwardVec.y << " " << m_curFrame.forwardVec.z << "\n"
        << m_curFrame.rightVec.x << " " << m_curFrame.rightVec.y << " " << m_curFrame.rightVec.z << "\n"
        << m_curFrame.upVec.x << " " << m_curFrame.upVec.y << " " << m_curFrame.upVec.z;

    std::string str = oss.str();
    fprintf(f, str.c_str());
    fclose(f);
}

void ObjectDetection::exportEgoObject(ObjEntity vPerspective) {
    FILE* f = fopen(m_egoObjectFilename.c_str(), "w");
    std::ostringstream oss;

    vPerspective.speed = ENTITY::GET_ENTITY_SPEED(vPerspective.entityID);

    exportEntity(vPerspective, oss, false, true, false);

    std::string str = oss.str();
    fprintf(f, str.c_str());
    fclose(f);
}

void ObjectDetection::exportCalib() {
    FILE* f = fopen(m_calibFilename.c_str(), "w");
    std::ostringstream oss;

    for (int i = 0; i <= 3; ++i) {
        oss << "P" << i << ": " <<
            m_curFrame.focalLen << " 0 " << (int)(s_camParams.width / 2) << " 0" <<
            " 0 " << m_curFrame.focalLen << " " << (int)(s_camParams.height / 2) << " 0" <<
            " 0 0 1 0\n";
    }
    oss << "R0_rect: 1 0 0 0 1 0 0 0 1\n" <<
        "Tr_velo_to_cam: 0 -1 0 0 0 0 -1 0 1 0 0 0\n" <<
        "Tr_imu_to_velo: 1 0 0 0 0 1 0 0 0 0 1 0";

    std::string str = oss.str();
    fprintf(f, str.c_str());
    fclose(f);
}

void ObjectDetection::exportDetections(FrameObjectInfo fObjInfo, ObjEntity* vPerspective) {
    if (collectTracking) {
        //TODO
    }
    FILE* f = fopen(m_labelsFilename.c_str(), "w");
    std::ostringstream oss;

    exportEntities(fObjInfo.vehicles, oss, false, false, true, OBJECT_MAX_DIST, 1, 1);
    exportEntities(fObjInfo.peds, oss, false, false, true, OBJECT_MAX_DIST, 1, 1);

    std::string str = oss.str();
    fprintf(f, str.c_str());
    fclose(f);

    if (OUTPUT_UNPROCESSED_LABELS) {
        f = fopen(m_labelsUnprocessedFilename.c_str(), "w");
        std::ostringstream oss1;

        exportEntities(fObjInfo.vehicles, oss1, true, false, true, OBJECT_MAX_DIST, 1, 1);
        exportEntities(fObjInfo.peds, oss1, true, false, true, OBJECT_MAX_DIST, 1, 1);

        std::string str1 = oss1.str();
        fprintf(f, str1.c_str());
        fclose(f);
    }

    f = fopen(m_labelsAugFilename.c_str(), "w");
    std::ostringstream oss2;

    //Augmented files also exports objects at any distance, with no 3D or 2D points
    exportEntities(fObjInfo.vehicles, oss2, false, true, false);
    exportEntities(fObjInfo.peds, oss2, false, true, false);

    std::string str2 = oss2.str();
    fprintf(f, str2.c_str());
    fclose(f);

    exportCalib();

    if (GENERATE_SECONDARY_PERSPECTIVES) {
        exportPosition();
        if (!vPerspective) {
            exportEgoObject(m_ownVehicleObj);
        }
        else {
            exportEgoObject(*vPerspective);
        }
    }
}

void ObjectDetection::exportImage(BYTE* data, std::string filename) {
    cv::Mat tempMat(cv::Size(s_camParams.width, s_camParams.height), CV_8UC3, data);
    if (filename.empty()) {
        filename = m_imgFilename;
    }
    cv::imwrite(filename, tempMat);
    tempMat.release();
}

Vector3 ObjectDetection::getGroundPoint(Vector3 point, Vector3 yVectorCam, Vector3 xVectorCam, Vector3 zVectorCam) {

    //The closer to the ground the less vehicle/world z discrepancy there will be
    //point.z -= (CAM_OFFSET_UP + CAR_CENTER_OFFSET_UP);

    Vector3 worldpoint = convertCoordinateSystem(point, yVectorCam, xVectorCam, zVectorCam);

    //Transition from relative to world
    worldpoint.x += s_camParams.pos.x;
    worldpoint.y += s_camParams.pos.y;
    worldpoint.z += s_camParams.pos.z;

    //Obtain groundz at world position
    float groundZ;
    GAMEPLAY::GET_GROUND_Z_FOR_3D_COORD(worldpoint.x, worldpoint.y, worldpoint.z, &(groundZ), 0);
    worldpoint.z = groundZ;

    worldpoint.x -= s_camParams.pos.x;
    worldpoint.y -= s_camParams.pos.y;
    worldpoint.z -= s_camParams.pos.z;

    Vector3 relPoint = convertCoordinateSystem(worldpoint, m_camForwardVector, m_camRightVector, m_camUpVector);

    return relPoint;
}

void ObjectDetection::setGroundPlanePoints() {
    //Distance to ground is ~1.73
    //See CAR_CENTER_OFFSET_UP
    /*float groundZ;
    GAMEPLAY::GET_GROUND_Z_FOR_3D_COORD(s_camParams.pos.x, s_camParams.pos.y, s_camParams.pos.z, &(groundZ), 0);
    float groundDiff = s_camParams.pos.z - groundZ;
    std::ostringstream osst;
    osst << "Distance to ground: " << groundDiff;
    log(osst.str(), true);*/

    //Vector of directions to test ground plane
    //Forward, right (up should always be 0 since it will be tested with get_ground_z
    std::vector<Vector3> points;
    points.push_back(createVec3(-2.f, 5.0, 0.f));
    points.push_back(createVec3(2.f, 5.0, 0.f));
    points.push_back(createVec3(0.f, 15.0, 0.f));
    points.push_back(createVec3(3.f, 15.0, 0.f));
    points.push_back(createVec3(-3.f, 15.0, 0.f));
    points.push_back(createVec3(-2, 25.0, 0));
    points.push_back(createVec3(2, 25.0, 0));

    //World coordinate vectors
    Vector3 worldX; worldX.x = 1; worldX.y = 0; worldX.z = 0;
    Vector3 worldY; worldY.x = 0; worldY.y = 1; worldY.z = 0;
    Vector3 worldZ; worldZ.x = 0; worldZ.y = 0; worldZ.z = 1;
    Vector3 xVectorCam = convertCoordinateSystem(worldX, m_camForwardVector, m_camRightVector, m_camUpVector);
    Vector3 yVectorCam = convertCoordinateSystem(worldY, m_camForwardVector, m_camRightVector, m_camUpVector);
    Vector3 zVectorCam = convertCoordinateSystem(worldZ, m_camForwardVector, m_camRightVector, m_camUpVector);

    //World coordinates have: y north, x east
    std::string filename = getStandardFilename("ground_points", ".txt");

    FILE* f = fopen(filename.c_str(), "w");
    std::ostringstream oss;

    for (auto point : points) {
        Vector3 relPoint = getGroundPoint(point, yVectorCam, xVectorCam, zVectorCam);

        //Output kitti velodyne coords relative position with ground z
        oss << relPoint.y << ", " << relPoint.x << ", " << relPoint.z << "\n";
    }
    std::string str = oss.str();
    fprintf(f, str.c_str());
    fclose(f);

    //**********************Creating ground point grid****************************************
    //World coordinates have: y north, x east
    std::string filename2 = getStandardFilename("ground_points_grid", ".txt");
    FILE* f2 = fopen(filename2.c_str(), "w");
    std::ostringstream oss2;

    int pointInterval = 2; //Distance between ground points (approximately in metres - game coordinates)
    for (int x = -MAX_LIDAR_DIST; x <= MAX_LIDAR_DIST; x += pointInterval) {
        for (int y = 0; y <= MAX_LIDAR_DIST; y += pointInterval) {
            Vector3 point = createVec3(x, y, 0.0f);
            Vector3 relPoint = getGroundPoint(point, yVectorCam, xVectorCam, zVectorCam);

            //Output kitti velodyne coords relative position with ground z
            oss2 << relPoint.y << ", " << relPoint.x << ", " << relPoint.z << "\n";
        }
    }

    std::string str2 = oss2.str();
    fprintf(f2, str2.c_str());
    fclose(f2);
}

Vector3 ObjectDetection::getVehicleDims(Entity e, Hash model, Vector3 &min, Vector3 &max) {
    GAMEPLAY::GET_MODEL_DIMENSIONS(model, &min, &max);

    //Calculate size
    Vector3 dim;
    dim.x = 0.5*(max.x - min.x);
    dim.y = 0.5*(max.y - min.y);
    dim.z = 0.5*(max.z - min.z);

    return dim;
}

//Entity p is the vehicle which is perceiving
void ObjectDetection::checkEntity(Vehicle p, WorldObject e, Vector3 pPos, std::ostringstream& oss) {
    if (p != e.e) {
        Vector3 forwardVector, rightVector, upVector, position;
        ENTITY::GET_ENTITY_MATRIX(e.e, &forwardVector, &rightVector, &upVector, &position); //Blue or red pill
        float distance = sqrt(SYSTEM::VDIST2(pPos.x, pPos.y, pPos.z, position.x, position.y, position.z));
        if (distance < 120) {
            bool losToCentre = ENTITY::HAS_ENTITY_CLEAR_LOS_TO_ENTITY(p, e.e, 19);

            Vector3 min, max;
            Vector3 dim = getVehicleDims(e.e, e.model, min, max);

            bool hasLOS = hasLOSToEntity(e.e, position, dim, forwardVector, rightVector, upVector, true, pPos);

            if (hasLOS || losToCentre) {
                SubsetInfo s = getObjectInfoSubset(position, forwardVector, dim);

                oss << e.type << " " << s.alpha_kitti << " " <<
                    s.kittiHeight << " " << s.kittiWidth << " " << s.kittiLength << " " <<
                    s.kittiPos.x << " " << s.kittiPos.y << " " << s.kittiPos.z << " " <<
                    s.rot_y << " " << distance << "\n";
            }
        }
    }
}

SubsetInfo ObjectDetection::getObjectInfoSubset(Vector3 position, Vector3 forwardVector, Vector3 dim) {
    SubsetInfo s;

    Vector3 relativePos;
    relativePos.x = position.x - s_camParams.pos.x;
    relativePos.y = position.y - s_camParams.pos.y;
    relativePos.z = position.z - s_camParams.pos.z;

    Vector3 kittiForwardVector = convertCoordinateSystem(forwardVector, m_camForwardVector, m_camRightVector, m_camUpVector);
    s.rot_y = -atan2(kittiForwardVector.y, kittiForwardVector.x);

    relativePos = convertCoordinateSystem(relativePos, m_camForwardVector, m_camRightVector, m_camUpVector);

    //Convert to KITTI camera coordinates
    s.kittiPos.x = relativePos.x;
    s.kittiPos.y = -relativePos.z;
    s.kittiPos.z = relativePos.y;

    //Kitti dimensions
    s.kittiHeight = 2 * dim.z;
    s.kittiWidth = 2 * dim.x;
    s.kittiLength = 2 * dim.y;

    //alpha is rot_y + tan^-1(z/x) + PI/2
    s.beta_kitti = atan2(s.kittiPos.z, s.kittiPos.x);
    s.alpha_kitti = s.rot_y + s.beta_kitti - PI / 2;

    return s;
}



void ObjectDetection::TrailersOnFrameID(std::vector<int>& TrailerArray) {
    const int ARR_SIZE = 1024;
    Vehicle vehicles[ARR_SIZE];

    int count = worldGetAllVehicles(vehicles, ARR_SIZE);
    for (int i = 0; i < count; i++) {
        if (vehicles[i] == m_ownVehicle) continue; //Don't process perspective car!

        Hash model = ENTITY::GET_ENTITY_MODEL(vehicles[i]);
        std::string modelString = VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(model);

        //List of trailers to be considered for towing purposes
        std::list<std::string> list_of_trailers = { "TRAILERLARGE", "TRAILER", "DOCKTRAILER","TR2","TANKER","TRAILERL","ARMYTRAILER","TR3","TRAILERS3"};
        std::list<std::string>::iterator it;
        it = std::find(list_of_trailers.begin(), list_of_trailers.end(), modelString);
        if (it != list_of_trailers.end()) //It's a car
        {
            TrailerArray.push_back(vehicles[i]);
        }
    }
}
