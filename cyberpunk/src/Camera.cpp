// Phase 5a: tell Minecraft where the game's camera is.
//
// To draw Minecraft's blocks in the right place, Minecraft has to look through the same camera as the game that
// draws the picture: same position, direction, tilt and field of view. Every frame this works out the camera and
// publishes it (Minecraft coordinates and degrees) to shared memory.
//
// There are three ways to find the camera's direction, and the game doesn't say which of them includes camera
// shake, head bob and hit reactions, so all three are computed, compared, and one is published:
//   transform  the active camera's world transform;
//   data       the camera system's "active camera data";
//   projected  the direction the picture really looks in, worked out from where the game's own world-to-screen
//              function puts points: three far-away points in known directions are projected, and the camera's
//              rotation that explains where they landed is solved for. Whatever moves the real picture (shake,
//              a hit, a ragdoll) moves those points on screen, so this sees it.
// The field of view is measured the same way, so it doesn't matter what convention the game's own number uses.
// Every two seconds the log says how much the three disagree.

#include "Camera.hpp"
#include "Depth.hpp"
#include "Ground.hpp"
#include "Link.hpp"
#include "Mapping.hpp"

#include <cybercraft_protocol.h>

#include <RED4ext/Scripting/Natives/ScriptGameInstance.hpp>
#include <RED4ext/Scripting/Natives/Transform.hpp>
#include <RED4ext/Scripting/Natives/Vector4.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace cybercraft::camera
{
	namespace
	{
		RED4ext::v1::PluginHandle g_handle = nullptr;
		const RED4ext::v1::Sdk* g_sdk = nullptr;
		constexpr double kPi = 3.14159265358979323846;
		constexpr double kDeg = 180.0 / kPi;

		struct V3
		{
			double x = 0, y = 0, z = 0;
		};
		V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		V3 operator*(V3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
		double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
		double Length(V3 a) { return std::sqrt(Dot(a, a)); }
		V3 Normalize(V3 a)
		{
			const double l = Length(a);
			return l > 1.0e-12 ? a * (1.0 / l) : V3{ 0, 0, 0 };
		}
		V3 Vec(const RED4ext::Vector4& v) { return { v.X, v.Y, v.Z }; }
		double AngleBetween(V3 a, V3 b) { return std::acos(std::clamp(Dot(Normalize(a), Normalize(b)), -1.0, 1.0)) * kDeg; }

		// A camera as three directions and a position, all in the game's own space (X east, Y north, Z up).
		struct Pose
		{
			bool ok = false;
			V3 pos;
			V3 fwd, right, up;
		};

		// The axes of a camera whose orientation is a quaternion (X right, Y forward, Z up).
		void QuatBasis(const RED4ext::Quaternion& q, V3& right, V3& fwd, V3& up)
		{
			const double i = q.i, j = q.j, k = q.k, r = q.r;
			right = { 1 - 2 * (j * j + k * k), 2 * (i * j + k * r), 2 * (i * k - j * r) };
			fwd = { 2 * (i * j - k * r), 1 - 2 * (i * i + k * k), 2 * (j * k + i * r) };
			up = { 2 * (i * k + j * r), 2 * (j * k - i * r), 1 - 2 * (i * i + j * j) };
		}

		RED4ext::IScriptable* g_system = nullptr;
		bool g_lookedUp = false;
		bool g_failed = false;
		RED4ext::CClassFunction* g_getTransform = nullptr;
		RED4ext::CClassFunction* g_getForward = nullptr;
		RED4ext::CClassFunction* g_getRight = nullptr;
		RED4ext::CClassFunction* g_getUp = nullptr;
		RED4ext::CClassFunction* g_getFov = nullptr;
		RED4ext::CClassFunction* g_getAspect = nullptr;
		RED4ext::CClassFunction* g_getData = nullptr;
		RED4ext::CClassFunction* g_project = nullptr;
		RED4ext::CClass* g_dataClass = nullptr;
		bool g_loggedDataLayout = false;

		int g_source = proto::kCamTransform;

		// How the game's ProjectPoint reports screen positions (worked out by Measure).
		struct Calibration
		{
			bool valid = false;
			float cx0 = 0, cy0 = 0;        // the value at the middle of the screen
			float scaleX = 1, scaleY = 1;  // half the range
			float sx = 1, sy = 1;          // +1 if x grows to the right / y grows upwards
		} g_cal;
		float g_measuredVfov = 0.0f;  // smoothed (0: none yet)
		float g_measuredHfov = 0.0f;

		// For predicting where the camera will be a moment from now.
		struct Motion
		{
			bool have = false;
			std::chrono::steady_clock::time_point time{};
			double x = 0, y = 0, z = 0;
			float yaw = 0, pitch = 0;
			float velX = 0, velY = 0, velZ = 0, yawRate = 0, pitchRate = 0;
		} g_motion;

		// Disagreement between the sources, reported every two seconds.
		struct Stats
		{
			std::chrono::steady_clock::time_point since{};
			double dataVsTransform = 0, projectedVsTransform = 0, projectedRoll = 0, projectedFit = 0;
			int frames = 0, projectedFailed = 0;
			// How far the transform-based camera model is from the game's own projection, over a grid of test points
			// (angle between the two view rays, degrees), by distance: 3 m, 12 m, 60 m.
			double gridMax[3] = { 0, 0, 0 };
			double gridCentre[3] = { 0, 0, 0 };
			double farSignedX = 0, farSignedZ = 0;  // mean signed error at 60 m: how far the model is rotated sideways / up
			int farSamples = 0, gridRuns = 0;
			// How far the position solved from near points is from the transform's position, metres.
			double shiftMax = 0;
			V3 shiftSum;
			int shiftSamples = 0;
		} g_stats;

		struct { bool valid = false; double x = 0, y = 0, z = 0, fx = 0, fy = 1, fz = 0; } g_lastPose;
		int g_frameCount = 0;
		bool g_loggedPublish = false;

		RED4ext::IScriptable* FindSystem()
		{
			auto rtti = RED4ext::CRTTISystem::Get();
			if (auto cls = rtti->GetClass("gameCameraSystem")) {
				auto engine = RED4ext::CGameEngine::Get();
				if (engine && engine->framework && engine->framework->gameInstance) {
					if (auto* system = engine->framework->gameInstance->GetSystem(cls)) {
						return system;
					}
				}
			}
			// The script way: GameInstance.GetCameraSystem(game).
			if (auto cls = rtti->GetClass("ScriptGameInstance")) {
				if (auto func = cls->GetFunction("GetCameraSystem")) {
					RED4ext::ScriptGameInstance game;
					RED4ext::Handle<RED4ext::IScriptable> system;
					RED4ext::StackArgs_t args;
					args.emplace_back(nullptr, &game);
					RED4ext::ExecuteFunction(static_cast<void*>(nullptr), func, &system, args);
					if (system) {
						return system.instance;
					}
				}
			}
			return nullptr;
		}

		bool Lookup()
		{
			g_lookedUp = true;
			g_system = FindSystem();
			if (!g_system) {
				g_sdk->logger->Error(g_handle, "camera: could not find the game's camera system; Minecraft's blocks can't be lined up");
				g_failed = true;
				return false;
			}
			auto* type = g_system->GetType();
			g_getTransform = type->GetFunction("GetActiveCameraWorldTransform");
			g_getForward = type->GetFunction("GetActiveCameraForward");
			g_getRight = type->GetFunction("GetActiveCameraRight");
			g_getUp = type->GetFunction("GetActiveCameraUp");
			g_getFov = type->GetFunction("GetActiveCameraFOV");
			g_getAspect = type->GetFunction("GetAspectRatio");
			g_getData = type->GetFunction("GetActiveCameraData");
			g_project = type->GetFunction("ProjectPoint");
			g_dataClass = RED4ext::CRTTISystem::Get()->GetClass("entCameraData");
			if (!g_getTransform || !g_getForward || !g_getRight || !g_getUp) {
				g_sdk->logger->Error(g_handle, "camera: the camera system lacks the transform, forward, right or up functions");
				g_failed = true;
				return false;
			}
			g_sdk->logger->InfoF(g_handle, "camera: found the game's camera system (projection %s, camera data %s)", g_project ? "available" : "NOT available",
				g_getData && g_dataClass ? "available" : "NOT available");
			return true;
		}

		bool CallVector(RED4ext::CClassFunction* a_func, RED4ext::Vector4& a_out)
		{
			return a_func && RED4ext::ExecuteFunction(g_system, a_func, &a_out);
		}

		bool CallFloat(RED4ext::CClassFunction* a_func, float& a_out)
		{
			return a_func && RED4ext::ExecuteFunction(g_system, a_func, &a_out);
		}

		bool Project(const V3& a_world, RED4ext::Vector4& a_screen)
		{
			RED4ext::Vector4 point(float(a_world.x), float(a_world.y), float(a_world.z), 1.0f);
			RED4ext::StackArgs_t args;
			args.emplace_back(nullptr, &point);
			return g_project && RED4ext::ExecuteFunction(g_system, g_project, &a_screen, args);
		}

		// Source 1: the transform.
		Pose ReadTransformPose()
		{
			Pose p;
			RED4ext::Transform transform{};
			bool result = false;
			RED4ext::StackArgs_t args;
			args.emplace_back(nullptr, &transform);
			if (!RED4ext::ExecuteFunction(g_system, g_getTransform, &result, args) || !result) {
				return p;
			}
			RED4ext::Vector4 f{}, r{}, u{};
			if (!CallVector(g_getForward, f) || !CallVector(g_getRight, r) || !CallVector(g_getUp, u)) {
				return p;
			}
			p.pos = Vec(transform.position);
			p.fwd = Normalize(Vec(f));
			p.right = Normalize(Vec(r));
			p.up = Normalize(Vec(u));
			p.ok = Length(p.fwd) > 0.5 && Length(p.right) > 0.5 && Length(p.up) > 0.5;
			return p;
		}

		// Source 2: the camera system's "active camera data", read through the game's reflection so no layout is assumed.
		Pose ReadDataPose(const Pose& a_fallbackPosition)
		{
			Pose p;
			if (!g_getData || !g_dataClass) {
				return p;
			}
			alignas(16) std::uint8_t buffer[0x200] = {};
			if (!RED4ext::ExecuteFunction(g_system, g_getData, buffer)) {
				return p;
			}
			bool havePos = false;
			bool haveRot = false;
			RED4ext::Vector4 position{};
			RED4ext::Quaternion rotation{};
			for (auto* cls = g_dataClass; cls; cls = cls->parent) {
				for (uint32_t i = 0; i < cls->props.Size(); ++i) {
					auto* prop = cls->props[i];
					const std::string type = prop->type ? prop->type->GetName().ToString() : "";
					const std::string name = prop->name.ToString();
					if (!g_loggedDataLayout) {
						g_sdk->logger->InfoF(g_handle, "camera: entCameraData has %s : %s", name.c_str(), type.c_str());
					}
					if (type == "Vector4" && !havePos && (name.find("os") != std::string::npos)) {
						position = prop->GetValue<RED4ext::Vector4>(buffer);
						havePos = true;
					} else if (type == "Quaternion" && !haveRot) {
						rotation = prop->GetValue<RED4ext::Quaternion>(buffer);
						haveRot = true;
					}
				}
			}
			g_loggedDataLayout = true;
			if (!haveRot) {
				return p;
			}
			QuatBasis(rotation, p.right, p.fwd, p.up);
			p.right = Normalize(p.right);
			p.fwd = Normalize(p.fwd);
			p.up = Normalize(p.up);
			p.pos = havePos ? Vec(position) : a_fallbackPosition.pos;
			p.ok = Length(p.fwd) > 0.5;
			return p;
		}

		// Projects three points: straight ahead, a little to the right, a little up. The screen distances between them
		// are the tangents of the half field of view, and the centre point says how the numbers are scaled (-1..1, 0..1 or
		// pixels) and whether they grow rightwards / upwards.
		bool Measure(const Pose& a_pose, bool a_log)
		{
			constexpr double kDistance = 10.0;
			constexpr double kOffset = 2.0;
			const V3 centreWorld = a_pose.pos + a_pose.fwd * kDistance;
			RED4ext::Vector4 c{}, r{}, u{};
			if (!Project(centreWorld, c) || !Project(centreWorld + a_pose.right * kOffset, r) || !Project(centreWorld + a_pose.up * kOffset, u)) {
				return false;
			}
			const bool ndc = std::fabs(c.X) < 0.1f && std::fabs(c.Y) < 0.1f;
			Calibration cal;
			cal.scaleX = ndc ? 1.0f : std::max(std::fabs(c.X), 1.0e-3f);
			cal.scaleY = ndc ? 1.0f : std::max(std::fabs(c.Y), 1.0e-3f);
			cal.cx0 = ndc ? 0.0f : cal.scaleX;
			cal.cy0 = ndc ? 0.0f : cal.scaleY;
			cal.sx = (r.X - c.X) >= 0 ? 1.0f : -1.0f;
			cal.sy = (u.Y - c.Y) >= 0 ? 1.0f : -1.0f;
			const float dx = std::fabs(r.X - c.X) / cal.scaleX;
			const float dy = std::fabs(u.Y - c.Y) / cal.scaleY;
			if (a_log) {
				g_sdk->logger->InfoF(g_handle, "camera: projection test: centre (%.3f, %.3f), 2 m right (%.3f, %.3f), 2 m up (%.3f, %.3f): %s numbers, x grows %s, y grows %s",
					c.X, c.Y, r.X, r.Y, u.X, u.Y, ndc ? "-1..1" : (c.X < 2.0f ? "0..1" : "pixel"), cal.sx > 0 ? "right" : "left", cal.sy > 0 ? "up" : "down");
			}
			if (dx < 0.02f || dy < 0.02f) {
				return false;
			}
			const float tanHalfH = float((kOffset / kDistance) / dx);
			const float tanHalfV = float((kOffset / kDistance) / dy);
			const float hfov = float(2.0 * std::atan(tanHalfH) * kDeg);
			const float vfov = float(2.0 * std::atan(tanHalfV) * kDeg);
			if (vfov < 20.0f || vfov > 140.0f || hfov < 20.0f || hfov > 170.0f) {
				return false;
			}
			cal.valid = true;
			g_cal = cal;
			// Straight from this frame's projection, not averaged: the field of view changes (loading, sprinting, zooming), and an
			// average takes seconds to catch up, during which every block is drawn a degree or two out.
			g_measuredVfov = vfov;
			g_measuredHfov = hfov;
			return true;
		}

		// Source 3: the camera's orientation as the picture really shows it. Projects three far points in known
		// directions, turns where they landed into directions in the camera's own space, and finds the rotation that
		// maps the known directions onto those (the TRIAD method). a_fit gets how far, in degrees, the third point is from
		// where that rotation predicts it (0 = a perfect fit).
		Pose ReadProjectedPose(const Pose& a_guess, double& a_fit)
		{
			Pose p;
			a_fit = 0;
			if (!g_project || !g_cal.valid || g_measuredHfov <= 0.0f) {
				return p;
			}
			const double tanHx = std::tan(g_measuredHfov * 0.5 / kDeg);
			const double tanHy = std::tan(g_measuredVfov * 0.5 / kDeg);
			const double kFar = 500.0;

			// Known directions (world) and the points they make.
			const V3 d[3] = { a_guess.fwd, Normalize(a_guess.fwd + a_guess.right * 0.5), Normalize(a_guess.fwd + a_guess.up * 0.5) };
			V3 v[3];  // the same directions as seen by the real camera, in its own space: x right, y FORWARD, z up (the same
			          // handedness as the world's east, north, up; right, up, forward would be mirrored and cannot be solved)
			for (int i = 0; i < 3; ++i) {
				RED4ext::Vector4 s{};
				if (!Project(a_guess.pos + d[i] * kFar, s)) {
					return p;
				}
				const double nx = g_cal.sx * (s.X - g_cal.cx0) / g_cal.scaleX;
				const double ny = g_cal.sy * (s.Y - g_cal.cy0) / g_cal.scaleY;
				if (std::fabs(nx) > 1.6 || std::fabs(ny) > 1.6) {
					return p;  // projected well outside the screen: not a number to trust
				}
				v[i] = Normalize({ nx * tanHx, 1.0, ny * tanHy });
			}

			// TRIAD from the first two directions.
			const V3 w1 = d[0];
			const V3 w2 = Normalize(Cross(d[0], d[1]));
			const V3 w3 = Cross(w1, w2);
			const V3 t1 = v[0];
			const V3 t2 = Normalize(Cross(v[0], v[1]));
			const V3 t3 = Cross(t1, t2);
			if (Length(w2) < 0.5 || Length(t2) < 0.5) {
				return p;
			}
			// R_wc = [t1 t2 t3] * [w1 w2 w3]^T, so that R_wc * w_i = t_i. Its rows are the camera's right, forward and up
			// in world space; row a is the a-th component of t1, t2 and t3 times w1, w2 and w3.
			const double tx[3] = { t1.x, t2.x, t3.x };
			const double ty[3] = { t1.y, t2.y, t3.y };
			const double tz[3] = { t1.z, t2.z, t3.z };
			const V3 w[3] = { w1, w2, w3 };
			auto rowOf = [&](int a) {
				const double* t = a == 0 ? tx : a == 1 ? ty : tz;
				return w[0] * t[0] + w[1] * t[1] + w[2] * t[2];
			};
			p.right = Normalize(rowOf(0));
			p.fwd = Normalize(rowOf(1));
			p.up = Normalize(rowOf(2));
			p.pos = a_guess.pos;

			// How well does this rotation predict the third point? (It was not used to build it.)
			const V3 predicted = Normalize({ Dot(p.right, d[2]), Dot(p.fwd, d[2]), Dot(p.up, d[2]) });
			a_fit = AngleBetween(predicted, v[2]);
			p.ok = Length(p.fwd) > 0.5;
			return p;
		}

		// The point closest to several lines (each: a point and a direction): the least-squares intersection.
		bool Intersect(const V3 a_points[], const V3 a_dirs[], int a_count, V3& a_out)
		{
			double A[3][3] = {};
			double b[3] = {};
			for (int k = 0; k < a_count; ++k) {
				const V3 d = Normalize(a_dirs[k]);
				const double m[3][3] = { { 1 - d.x * d.x, -d.x * d.y, -d.x * d.z }, { -d.y * d.x, 1 - d.y * d.y, -d.y * d.z }, { -d.z * d.x, -d.z * d.y, 1 - d.z * d.z } };
				const double q[3] = { a_points[k].x, a_points[k].y, a_points[k].z };
				for (int i = 0; i < 3; ++i) {
					for (int j = 0; j < 3; ++j) {
						A[i][j] += m[i][j];
						b[i] += m[i][j] * q[j];
					}
				}
			}
			const double det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) + A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
			if (std::fabs(det) < 1.0e-6) {
				return false;
			}
			auto replaceColumn = [&](int col) {
				double M[3][3];
				std::memcpy(M, A, sizeof(M));
				for (int i = 0; i < 3; ++i) {
					M[i][col] = b[i];
				}
				return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
			};
			a_out = { replaceColumn(0) / det, replaceColumn(1) / det, replaceColumn(2) / det };
			return true;
		}

		// Normalised screen position from the game's ProjectPoint, as a direction in the camera's own space
		// (x right, y forward, z up), or false if the point is behind the camera or well off the screen.
		bool ProjectedRay(const V3& a_world, V3& a_ray)
		{
			RED4ext::Vector4 s{};
			if (!Project(a_world, s)) {
				return false;
			}
			const double nx = g_cal.sx * (s.X - g_cal.cx0) / g_cal.scaleX;
			const double ny = g_cal.sy * (s.Y - g_cal.cy0) / g_cal.scaleY;
			if (std::fabs(nx) > 1.6 || std::fabs(ny) > 1.6) {
				return false;
			}
			const double tanHx = std::tan(g_measuredHfov * 0.5 / kDeg);
			const double tanHy = std::tan(g_measuredVfov * 0.5 / kDeg);
			a_ray = Normalize({ nx * tanHx, 1.0, ny * tanHy });
			return true;
		}

		// Source 4: the rotation from far points (ReadProjectedPose), then the camera's position from points close to it.
		// Close points are very sensitive to where the camera really is, far ones only to where it points, so together
		// they give both. a_shift gets how far the position moved from the guess.
		Pose ReadProjectedPositionPose(const Pose& a_guess, const Pose& a_rotation, V3& a_shift)
		{
			Pose p = a_rotation;
			a_shift = {};
			if (!a_rotation.ok || !g_project || !g_cal.valid) {
				p.ok = false;
				return p;
			}
			constexpr double kNear = 4.0;
			const V3 d[3] = { a_guess.fwd, Normalize(a_guess.fwd + a_guess.right * 0.5), Normalize(a_guess.fwd + a_guess.up * 0.5) };
			V3 points[3];
			V3 dirs[3];
			for (int i = 0; i < 3; ++i) {
				V3 ray;
				points[i] = a_guess.pos + d[i] * kNear;
				if (!ProjectedRay(points[i], ray)) {
					p.ok = false;
					return p;
				}
				// The same ray in world space, using the rotation just found.
				dirs[i] = a_rotation.right * ray.x + a_rotation.fwd * ray.y + a_rotation.up * ray.z;
			}
			V3 position;
			if (!Intersect(points, dirs, 3, position)) {
				p.ok = false;
				return p;
			}
			a_shift = position - a_guess.pos;
			if (Length(a_shift) > 1.0) {
				p.ok = false;  // not believable: a camera is not a metre away from where it says it is
				return p;
			}
			p.pos = position;
			return p;
		}

		// How well does the transform-based camera (position, directions, measured field of view) predict where the
		// game's own projection puts a grid of test points? The angle between the two view rays, at 3, 12 and 60 m.
		void GridCheck(const Pose& a_model)
		{
			if (!g_project || !g_cal.valid || g_measuredHfov <= 0.0f) {
				return;
			}
			const double tanHx = std::tan(g_measuredHfov * 0.5 / kDeg);
			const double tanHy = std::tan(g_measuredVfov * 0.5 / kDeg);
			const double distances[3] = { 3.0, 12.0, 60.0 };
			const double azimuths[3] = { -35.0, 0.0, 35.0 };
			const double elevations[3] = { -20.0, 0.0, 20.0 };
			bool any = false;
			for (int di = 0; di < 3; ++di) {
				for (double az : azimuths) {
					for (double el : elevations) {
						const double a = az / kDeg;
						const double e = el / kDeg;
						const V3 dir = a_model.fwd * (std::cos(e) * std::cos(a)) + a_model.right * (std::cos(e) * std::sin(a)) + a_model.up * std::sin(e);
						const V3 point = a_model.pos + dir * distances[di];
						V3 observed;
						if (!ProjectedRay(point, observed)) {
							continue;
						}
						// What the model says: the point relative to the camera, in the camera's own space.
						const V3 rel = point - a_model.pos;
						const double x = Dot(rel, a_model.right);
						const double y = Dot(rel, a_model.fwd);
						const double z = Dot(rel, a_model.up);
						if (y <= 0.01) {
							continue;
						}
						// Through the same field of view the observation went through, so only a difference in where the camera is
						// and which way it points shows up, not the field of view's own rounding.
						const V3 predicted = Normalize({ (x / y / tanHx) * tanHx, 1.0, (z / y / tanHy) * tanHy });
						const double error = AngleBetween(predicted, observed);
						g_stats.gridMax[di] = std::max(g_stats.gridMax[di], error);
						if (az == 0.0 && el == 0.0) {
							g_stats.gridCentre[di] = std::max(g_stats.gridCentre[di], error);
							if (di == 2) {
								g_stats.farSignedX += std::atan2(observed.x - predicted.x, 1.0) * kDeg;
								g_stats.farSignedZ += std::atan2(observed.z - predicted.z, 1.0) * kDeg;
								++g_stats.farSamples;
							}
						}
						any = true;
					}
				}
			}
			if (any) {
				++g_stats.gridRuns;
			}
		}

		// What does ProjectPoint return in its third and fourth numbers? Points straight ahead at growing distances: if the third is
		// the depth value, it will follow the distance in a way that shows the near plane and whether depth is reversed.
		void LogProjectionDepths(const Pose& a_pose)
		{
			if (!g_project) {
				return;
			}
			const double distances[] = { 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 300.0, 1000.0 };
			std::string line = "camera: ProjectPoint (x, y, z, w) for points straight ahead:";
			char text[160];
			for (double d : distances) {
				RED4ext::Vector4 s{};
				if (Project(a_pose.pos + a_pose.fwd * d, s)) {
					std::snprintf(text, sizeof(text), " | %gm: %.5f %.5f %.6f %.5f", d, s.X, s.Y, s.Z, s.W);
				} else {
					std::snprintf(text, sizeof(text), " | %gm: failed", d);
				}
				line += text;
			}
			g_sdk->logger->Info(g_handle, line.c_str());
		}

		// For the depth capture: how far along the view the game's world is at five places on the screen, measured with the game's own
		// rays. (The capture reads the depth texture at the same places; the two should agree once the right depth is being copied.)
		void MeasureDepthReferences(const Pose& a_pose)
		{
			if (g_measuredHfov <= 0.0f) {
				return;
			}
			const double tanHx = std::tan(g_measuredHfov * 0.5 / kDeg);
			const double tanHy = std::tan(g_measuredVfov * 0.5 / kDeg);
			const float places[depth::kReferenceCount][2] = { { 0.5f, 0.5f }, { 0.25f, 0.5f }, { 0.75f, 0.5f }, { 0.5f, 0.25f }, { 0.5f, 0.75f } };
			depth::Reference refs[depth::kReferenceCount];
			for (int i = 0; i < depth::kReferenceCount; ++i) {
				const double ndcX = 2.0 * places[i][0] - 1.0;
				const double ndcY = 1.0 - 2.0 * places[i][1];
				const double sx = ndcX * tanHx;
				const double sy = ndcY * tanHy;
				const V3 dir = a_pose.fwd + a_pose.right * sx + a_pose.up * sy;
				const double distance = ground::RayDistance(a_pose.pos.x, a_pose.pos.y, a_pose.pos.z, dir.x, dir.y, dir.z, 300.0);
				refs[i].u = places[i][0];
				refs[i].v = places[i][1];
				if (!std::isnan(distance)) {
					// Distance along the ray -> distance along the view direction (what a depth buffer holds).
					refs[i].z = static_cast<float>(distance / std::sqrt(1.0 + sx * sx + sy * sy));
					refs[i].valid = true;
				}
			}
			depth::SetReferences(refs, depth::kReferenceCount);
		}

		// Cyberpunk (X east, Y north, Z up) -> Minecraft (X east, Y up, Z south).
		V3 ToMinecraft(V3 a)
		{
			return { a.x, a.z, -a.y };
		}
	}

	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk)
	{
		g_handle = a_handle;
		g_sdk = a_sdk;
	}

	void SetSource(int a_source)
	{
		a_source = std::clamp(a_source, 0, 3);
		if (a_source != g_source) {
			g_source = a_source;
			const char* names[] = { "the transform", "the camera data", "the projected orientation", "the projected orientation and position" };
			g_sdk->logger->InfoF(g_handle, "camera: now publishing %s", names[a_source]);
		}
	}

	bool LastPose(double& a_x, double& a_y, double& a_z, double& a_fx, double& a_fy, double& a_fz)
	{
		a_x = g_lastPose.x;
		a_y = g_lastPose.y;
		a_z = g_lastPose.z;
		a_fx = g_lastPose.fx;
		a_fy = g_lastPose.fy;
		a_fz = g_lastPose.fz;
		return g_lastPose.valid;
	}

	void Reset()
	{
		g_system = nullptr;
		g_lookedUp = false;
		g_failed = false;
		g_cal = Calibration{};
		g_measuredVfov = 0.0f;
		g_measuredHfov = 0.0f;
		g_motion.have = false;
	}

	void Update(RED4ext::Handle<RED4ext::IScriptable>& a_player)
	{
		(void)a_player;
		auto& link = Link::Get();
		if (g_failed || !link.IsOpen()) {
			return;
		}
		if (!g_lookedUp && !Lookup()) {
			return;
		}

		const Pose transformPose = ReadTransformPose();
		if (!transformPose.ok) {
			link.PublishCamera(false, 0, 0, 0, 0, 0, 60.0f, 1.0f);
			return;
		}
		++g_frameCount;
		g_lastPose = { true, transformPose.pos.x, transformPose.pos.y, transformPose.pos.z, transformPose.fwd.x, transformPose.fwd.y, transformPose.fwd.z };
		const auto now = std::chrono::steady_clock::now();
		const bool logNow = now - g_stats.since >= std::chrono::seconds(2);

		float gameFov = 0.0f;
		float aspect = 0.0f;
		CallFloat(g_getFov, gameFov);
		CallFloat(g_getAspect, aspect);
		if (aspect < 0.5f || aspect > 4.0f) {
			aspect = 16.0f / 9.0f;
		}

		// The field of view and how the projection numbers are scaled: measured on the first frames, then now and then.
		if (g_project) {
			Measure(transformPose, !g_cal.valid && g_frameCount < 40);
		}

		// The other two sources cost a few calls each: every frame if one is chosen, otherwise now and then, for the log.
		const bool needOthers = g_source != proto::kCamTransform || g_frameCount % 10 == 0;
		Pose dataPose;
		Pose projectedPose;
		Pose positionedPose;
		V3 positionShift;
		double fit = 0;
		if (needOthers) {
			dataPose = ReadDataPose(transformPose);
			projectedPose = ReadProjectedPose(transformPose, fit);
			positionedPose = ReadProjectedPositionPose(transformPose, projectedPose, positionShift);
		}
		if (g_frameCount % 30 == 0) {
			GridCheck(transformPose);
		}
		if (g_frameCount == 120 || g_frameCount == 2400) {
			LogProjectionDepths(transformPose);
		}
		if (depth::CaptureWanted() && g_frameCount % 45 == 0) {
			MeasureDepthReferences(transformPose);
		}

		// Statistics for the log.
		if (needOthers) {
			++g_stats.frames;
		}
		if (dataPose.ok) {
			g_stats.dataVsTransform = std::max(g_stats.dataVsTransform, AngleBetween(dataPose.fwd, transformPose.fwd));
		}
		if (projectedPose.ok) {
			g_stats.projectedVsTransform = std::max(g_stats.projectedVsTransform, AngleBetween(projectedPose.fwd, transformPose.fwd));
			const V3 pr = ToMinecraft(projectedPose.right);
			const V3 pu = ToMinecraft(projectedPose.up);
			g_stats.projectedRoll = std::max(g_stats.projectedRoll, std::fabs(std::atan2(pr.y, pu.y) * kDeg));
			g_stats.projectedFit = std::max(g_stats.projectedFit, fit);
		} else if (needOthers) {
			++g_stats.projectedFailed;
		}
		if (positionedPose.ok) {
			g_stats.shiftMax = std::max(g_stats.shiftMax, Length(positionShift));
			g_stats.shiftSum = g_stats.shiftSum + positionShift;
			++g_stats.shiftSamples;
		}

		// Choose what to publish.
		Pose chosen = transformPose;
		if (g_source == proto::kCamData && dataPose.ok) {
			chosen = dataPose;
		} else if (g_source == proto::kCamProjected && projectedPose.ok && fit < 2.0) {
			chosen = projectedPose;
		} else if (g_source == proto::kCamProjectedPos && positionedPose.ok && fit < 2.0) {
			chosen = positionedPose;
		}

		// The game's own number, as a fallback: take it as the vertical field of view in degrees.
		float vfov = g_measuredVfov;
		if (vfov <= 0.0f) {
			vfov = gameFov > 20.0f && gameFov < 140.0f ? gameFov : 60.0f;
		}

		const V3 f = ToMinecraft(chosen.fwd);
		const V3 r = ToMinecraft(chosen.right);
		const V3 u = ToMinecraft(chosen.up);
		V3 pos = ToMinecraft(chosen.pos);
		pos.y -= mapping::Offset();  // Minecraft's height = the game's height - the vertical offset
		const double length = Length(f);
		if (length < 1.0e-6) {
			link.PublishCamera(false, 0, 0, 0, 0, 0, vfov, aspect);
			return;
		}
		const V3 dir = f * (1.0 / length);
		const float yaw = static_cast<float>(std::atan2(-dir.x, dir.z) * kDeg);
		const float pitch = static_cast<float>(-std::asin(std::clamp(dir.y, -1.0, 1.0)) * kDeg);
		// Tilt: how far the camera's right/up are from level (0 when the horizon is level).
		const float roll = static_cast<float>(std::atan2(r.y, u.y) * kDeg);

		// How fast the camera moves and turns (smoothed): Minecraft can draw a little ahead of it if asked to.
		if (g_motion.have) {
			const double dt = std::chrono::duration<double>(now - g_motion.time).count();
			if (dt > 0.0005 && dt < 0.25) {
				auto wrap = [](float a) {
					while (a > 180.0f) a -= 360.0f;
					while (a < -180.0f) a += 360.0f;
					return a;
				};
				const float blend = 0.5f;
				g_motion.velX += (float((pos.x - g_motion.x) / dt) - g_motion.velX) * blend;
				g_motion.velY += (float((pos.y - g_motion.y) / dt) - g_motion.velY) * blend;
				g_motion.velZ += (float((pos.z - g_motion.z) / dt) - g_motion.velZ) * blend;
				g_motion.yawRate += (wrap(yaw - g_motion.yaw) / float(dt) - g_motion.yawRate) * blend;
				g_motion.pitchRate += ((pitch - g_motion.pitch) / float(dt) - g_motion.pitchRate) * blend;
			}
		}
		g_motion.have = true;
		g_motion.time = now;
		g_motion.x = pos.x;
		g_motion.y = pos.y;
		g_motion.z = pos.z;
		g_motion.yaw = yaw;
		g_motion.pitch = pitch;

		link.PublishCamera(true, pos.x, pos.y, pos.z, yaw, pitch, vfov, aspect, g_motion.velX, g_motion.velY, g_motion.velZ, g_motion.yawRate, g_motion.pitchRate, roll);

		if (!g_loggedPublish) {
			g_loggedPublish = true;
			g_sdk->logger->Info(g_handle, "camera: publishing the game's camera to Minecraft");
		}
		if (logNow) {
			g_sdk->logger->InfoF(g_handle,
				"camera: last 2 s (publishing %s): position (%.2f, %.2f, %.2f); the game's fov number %.2f, measured vertical %.2f horizontal %.2f, aspect %.3f; "
				"largest disagreement in direction: camera data vs transform %.2f deg, projected vs transform %.2f deg (projected tilt up to %.2f deg, fit error up to %.3f deg, %d of %d frames without a projected result)",
				g_source == 0 ? "the transform" : g_source == 1 ? "the camera data" : g_source == 2 ? "the projected orientation" : "the projected orientation and position", chosen.pos.x, chosen.pos.y, chosen.pos.z, gameFov,
				g_measuredVfov, g_measuredHfov, aspect, g_stats.dataVsTransform, g_stats.projectedVsTransform, g_stats.projectedRoll, g_stats.projectedFit,
				g_stats.projectedFailed, g_stats.frames);
			if (g_stats.gridRuns > 0) {
				const double n = std::max(1, g_stats.farSamples);
				g_sdk->logger->InfoF(g_handle,
					"camera: check against the game's own projection (transform camera; angle between the two view rays, largest over a grid of 9 directions): at 3 m %.3f deg, at 12 m %.3f deg, at 60 m %.3f deg; "
					"straight ahead: %.3f, %.3f, %.3f deg; at 60 m the model is on average %.3f deg too far right and %.3f deg too far up",
					g_stats.gridMax[0], g_stats.gridMax[1], g_stats.gridMax[2], g_stats.gridCentre[0], g_stats.gridCentre[1], g_stats.gridCentre[2],
					-g_stats.farSignedX / n, -g_stats.farSignedZ / n);
			}
			if (g_stats.shiftSamples > 0) {
				const double n = g_stats.shiftSamples;
				g_sdk->logger->InfoF(g_handle,
					"camera: position solved from points near the camera differs from the transform's by up to %.3f m (on average %.3f east, %.3f north, %.3f up)",
					g_stats.shiftMax, g_stats.shiftSum.x / n, g_stats.shiftSum.y / n, g_stats.shiftSum.z / n);
			}
			g_stats = Stats{};
			g_stats.since = now;
		}
	}
}
