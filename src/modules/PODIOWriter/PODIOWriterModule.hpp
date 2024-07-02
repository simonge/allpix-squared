/**
 * @file
 * @brief Definition of [PODIOWriter] module
 *
 * @copyright Copyright (c) 2017-2024 CERN and the Allpix Squared authors.
 * This software is distributed under the terms of the MIT License, copied verbatim in the file "LICENSE.md".
 * In applying this license, CERN does not waive the privileges and immunities granted to it by virtue of its status as an
 * Intergovernmental Organization or submit itself to any jurisdiction.
 * SPDX-License-Identifier: MIT
 */

#include <string>
#include <vector>

#include "core/config/Configuration.hpp"
#include "core/geometry/GeometryManager.hpp"
#include "core/messenger/Messenger.hpp"
#include "core/module/Event.hpp"
#include "core/module/Module.hpp"

#include "objects/PixelHit.hpp"

#include <podio/EventStore.h>
#include <podio/ROOTWriter.h>

namespace allpix {

    /**
     * @ingroup Modules
     * @brief Module to write hit data to a edm4allpix.root file
     *
     * Create PODIO file, compatible to EUTelescope analysis framework.
     */
    class PODIOWriterModule : public SequentialModule {
    public:
        /**
         * @brief Constructor for this unique module
         * @param config Configuration object for this module as retrieved from the steering file
         * @param messenger Pointer to the messenger object to allow binding to messages on the bus
         * @param geo_manager Pointer to the geometry manager, containing the detectors
         */
        PODIOWriterModule(Configuration& config, Messenger* messenger, GeometryManager* geo_manager);

        /**
         * @brief Initialize PODIO/edm4allpix output files
         */
        void initialize() override;

        /**
         * @brief Receive pixel hit messages, create lcio event, add hit collection and write event to file.
         */
        void run(Event* event) override;

        /**
         * @brief Close the output file
         */
        void finalize() override;

    private:
        Messenger* messenger_;
        GeometryManager* geo_mgr_{};  
        std::unique_ptr<podio::EventStore> event_store_;
        std::unique_ptr<podio::ROOTWriter> writer_;

    };
} // namespace allpix
