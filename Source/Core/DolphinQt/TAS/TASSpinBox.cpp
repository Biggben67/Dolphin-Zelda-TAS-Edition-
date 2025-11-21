// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/TAS/TASSpinBox.h"

#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "Core/HotkeyManager.h"

TASSpinBox::TASSpinBox(QWidget* parent) : QSpinBox(parent)
{
  connect(this, QOverload<int>::of(&TASSpinBox::valueChanged), this, &TASSpinBox::OnUIValueChanged);
}

int TASSpinBox::GetValue() const
{
  return m_state.GetValue();
}

void TASSpinBox::OnControllerValueChanged(int new_value)
{
  if (m_state.OnControllerValueChanged(new_value))
    QueueOnObject(this, &TASSpinBox::ApplyControllerValueChange);
}

void TASSpinBox::OnUIValueChanged(int new_value)
{
  m_state.OnUIValueChanged(new_value);
}

void TASSpinBox::ApplyControllerValueChange()
{
  setValue(m_state.ApplyControllerValueChange());
}

void TASSpinBox::focusInEvent(QFocusEvent* event)
{
  QSpinBox::focusInEvent(event);
  HotkeyManagerEmu::SetStateHotkeysBlocked(true);
}

void TASSpinBox::focusOutEvent(QFocusEvent* event)
{
  QSpinBox::focusOutEvent(event);
  HotkeyManagerEmu::SetStateHotkeysBlocked(false);
}
