// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/Mapping/FreeLookKeyframes.h"

#include <QGroupBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "Core/FreeLookManager.h"

#include "DolphinQt/Config/FreeLookKeyframeEditor.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"

#include "InputCommon/InputConfig.h"

FreeLookKeyframes::FreeLookKeyframes(MappingWindow* window) : MappingWidget(window)
{
  CreateMainLayout();
}

void FreeLookKeyframes::CreateMainLayout()
{
  m_open_editor_button = new NonDefaultQPushButton(tr("Open Keyframe Editor"));
  connect(m_open_editor_button, &QPushButton::clicked, this, &FreeLookKeyframes::OpenEditor);

  auto* editor_group = new QGroupBox(tr("Keyframe Editor"));
  auto* editor_layout = new QVBoxLayout();
  editor_layout->addWidget(m_open_editor_button);
  editor_group->setLayout(editor_layout);

  auto* layout = new QVBoxLayout();
  layout->addWidget(
      CreateGroupBox(tr("Controls"), FreeLook::GetInputGroup(GetPort(), FreeLookGroup::Keyframe)));
  layout->addWidget(editor_group);
  layout->addStretch();
  setLayout(layout);
}

void FreeLookKeyframes::OpenEditor()
{
  auto* editor = new FreeLookKeyframeEditor(this);
  editor->setAttribute(Qt::WA_DeleteOnClose, true);
  editor->setWindowModality(Qt::WindowModality::WindowModal);
  editor->show();
}

InputConfig* FreeLookKeyframes::GetConfig()
{
  return FreeLook::GetInputConfig();
}

void FreeLookKeyframes::LoadSettings()
{
  FreeLook::LoadInputConfig();
}

void FreeLookKeyframes::SaveSettings()
{
  FreeLook::GetInputConfig()->SaveConfig();
}
