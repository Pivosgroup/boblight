/*
 * boblight
 * Copyright (C) tim.helloworld 2013
 * 
 * boblight is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * boblight is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef CDEVICEUSB
#define CDEVICEUSB 

#include "device.h"

class CDeviceUsb: public CDevice
{
  public:
    CDeviceUsb(CClientsHandler& clients): CDevice(clients), m_busnumber(-1), m_deviceaddress(-1) {}

    void SetBusNumber(int num)         { m_busnumber = num;         }
    void SetDeviceAddress(int address) { m_deviceaddress = address; }

    int  m_busnumber;
    int  m_deviceaddress;
};

#endif //CDEVICEUSB 
