/****************************************************************************
 * arch/c33/src/s1c33e07/s1c33e07_spi.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __ARCH_C33_SRC_S1C33E07_S1C33E07_SPI_H
#define __ARCH_C33_SRC_S1C33E07_S1C33E07_SPI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/spi/spi.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: s1c33e07_spibus_initialize
 *
 * Description:
 *   Initialize the one SPI master the chip has and return its interface.
 *   Chip selects live on port 5 and belong to the board, which supplies
 *   s1c33e07_spiselect() and s1c33e07_spistatus() below.
 *
 * Input Parameters:
 *   port - Must be 0; there is no second controller.
 *
 * Returned Value:
 *   A pointer to the SPI interface, or NULL for any other port.
 *
 ****************************************************************************/

struct spi_dev_s *s1c33e07_spibus_initialize(int port);

/* Both are implemented by the board, which knows which pin selects what. */

void s1c33e07_spiselect(struct spi_dev_s *dev, uint32_t devid,
                        bool selected);
uint8_t s1c33e07_spistatus(struct spi_dev_s *dev, uint32_t devid);

#endif /* __ARCH_C33_SRC_S1C33E07_S1C33E07_SPI_H */
