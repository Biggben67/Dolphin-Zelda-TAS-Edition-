// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "Common/Matrix.h"
#include "Core/Config/FreeLookSettings.h"

class PointerWrap;

namespace Core
{
class System;
}

class CameraController
{
public:
  CameraController() = default;
  virtual ~CameraController() = default;

  CameraController(const CameraController&) = delete;
  CameraController& operator=(const CameraController&) = delete;

  CameraController(CameraController&&) = delete;
  CameraController& operator=(CameraController&&) = delete;

  virtual Common::Matrix44 GetView() const = 0;
  virtual Common::Vec2 GetFieldOfViewMultiplier() const = 0;

  virtual void DoState(PointerWrap& p) = 0;

  virtual bool IsDirty() const = 0;
  virtual void SetClean() = 0;

  virtual bool SupportsInput() const = 0;
};

class CameraControllerInput : public CameraController
{
public:
  enum class KeyframeInterpolationMode : int
  {
    Linear = 0,
    CatmullRom = 1,
    SmoothStep = 2
  };

  struct KeyframeData
  {
    uint64_t frame = 0;
    Common::Vec3 position = Common::Vec3{};
    Common::Vec3 rotation = Common::Vec3{};
    Common::Vec2 fov = Common::Vec2{1.0f, 1.0f};
  };

  Common::Vec2 GetFieldOfViewMultiplier() const final;

  void DoState(PointerWrap& p) override;

  bool IsDirty() const final { return m_dirty; }
  void SetClean() final { m_dirty = false; }

  bool SupportsInput() const final { return true; }

  virtual void MoveVertical(float amt) = 0;
  virtual void MoveHorizontal(float amt) = 0;

  virtual void MoveForward(float amt) = 0;

  virtual void Rotate(const Common::Vec3& amt) = 0;
  virtual void Rotate(const Common::Quaternion& quat) = 0;

  virtual void Reset() = 0;

  virtual bool SupportsKeyframeAnimation() const;
  virtual std::size_t GetKeyframeCount() const;
  virtual bool IsKeyframePlaybackActive() const;
  virtual void AddKeyframe();
  virtual void DeleteLastKeyframe();
  virtual void ClearKeyframes();
  virtual void ToggleKeyframePlayback();
  virtual bool SaveKeyframesToFile();
  virtual bool LoadKeyframesFromFile();
  virtual bool SaveKeyframesToDraftFile();
  virtual bool LoadKeyframesFromDraftFile();
  virtual void AdvanceKeyframePlayback(float dt);
  virtual void UpdateFocusTarget();
  virtual std::vector<KeyframeData> GetKeyframes() const;
  virtual bool GetKeyframe(std::size_t index, KeyframeData* out_keyframe) const;
  virtual bool UpdateKeyframe(std::size_t index, const KeyframeData& keyframe);
  virtual bool DeleteKeyframe(std::size_t index);
  virtual bool ReplaceKeyframes(std::vector<KeyframeData> keyframes,
                                KeyframeInterpolationMode interpolation_mode,
                                bool focus_relative);
  virtual KeyframeData GetCurrentCameraTransform() const;
  virtual KeyframeInterpolationMode GetKeyframeInterpolationMode() const;
  virtual void SetKeyframeInterpolationMode(KeyframeInterpolationMode mode);

  void IncreaseFovX(float fov);
  void IncreaseFovY(float fov);
  float GetFovStepSize() const;

  void ModifySpeed(float multiplier);
  void ResetSpeed();
  float GetSpeed() const;

protected:
  void MarkDirty();
  void SetFieldOfViewMultiplier(const Common::Vec2& multiplier);

private:
  static constexpr float MIN_FOV_MULTIPLIER = 0.025f;
  static constexpr float DEFAULT_SPEED = 60.0f;
  static constexpr float DEFAULT_FOV_MULTIPLIER = 1.0f;

  float m_fov_x_multiplier = DEFAULT_FOV_MULTIPLIER;
  float m_fov_y_multiplier = DEFAULT_FOV_MULTIPLIER;
  float m_speed = DEFAULT_SPEED;
  bool m_dirty = false;
};

class FreeLookCamera
{
public:
  FreeLookCamera();

  void RefreshConfig();

  Common::Matrix44 GetView() const;
  Common::Vec2 GetFieldOfViewMultiplier() const;

  void DoState(PointerWrap& p);

  bool IsActive() const;

  CameraController* GetController() const;
  void UpdateFocusTargetFromMemory(Core::System& system);

private:
  std::unique_ptr<CameraController> m_camera_controller;

  bool m_is_enabled{};
  std::optional<FreeLook::ControlType> m_current_type;
};

extern FreeLookCamera g_freelook_camera;
