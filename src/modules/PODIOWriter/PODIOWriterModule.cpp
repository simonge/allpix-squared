/**
 * @file
 * @brief Implementation of [PODIOWriter] module
 *
 * @copyright Copyright (c) 2017-2024 CERN and the Allpix Squared authors.
 * This software is distributed under the terms of the MIT License, copied verbatim in the file "LICENSE.md".
 * In applying this license, CERN does not waive the privileges and immunities granted to it by virtue of its status as an
 * Intergovernmental Organization or submit itself to any jurisdiction.
 * SPDX-License-Identifier: MIT
 */

#include "PODIOWriterModule.hpp"

#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "core/messenger/Messenger.hpp"
#include "core/utils/log.h"

using namespace allpix;
using namespace edm4allpix;

PODIOWriterModule::PODIOWriterModule(Configuration& config, Messenger* messenger, GeometryManager* geo)
    : SequentialModule(config), messenger_(messenger), geo_mgr_(geo) {
    // Enable multithreading of this module if multithreading is enabled
    allow_multithreading();

    // Set configuration defaults:
    config_.setDefault("file_name", "output.edm4allpix.root");

    pixel_type_ = config_.get<int>("pixel_type");
    detector_name_ = config_.get<std::string>("detector_name");
    dump_mc_truth_ = config_.get<bool>("dump_mc_truth");
    // There are two ways to configure this module - either by providing a "output_collection_name" or a
    // "detector_assignment". Throws an error if both are provided and defaults back to "output_collection_name" if none are
    // provided
    auto has_short_config = config_.has("output_collection_name");
    auto has_long_config = config_.has("detector_assignment");

    auto detectors = geo_mgr_->getDetectors();

}

void PODIOWriterModule::initialize() {
    // Create the output GEAR file for the detector geometry
    geometry_file_name_ = createOutputFile(config_.get<std::string>("geometry_file"), "xml");
    // Open LCIO file and write run header
    lcio_file_name_ = createOutputFile(config_.get<std::string>("file_name"), "slcio");
    lcWriter_ = std::shared_ptr<IO::LCWriter>(LCFactory::getInstance()->createLCWriter());
    lcWriter_->open(lcio_file_name_, LCIO::WRITE_NEW);
    auto run = std::make_unique<LCRunHeaderImpl>();
    run->setRunNumber(1);
    run->setDetectorName(detector_name_);
    lcWriter_->writeRunHeader(run.get());

    event_store_ = std::make_unique<podio::EventStore>();
    writer_ = std::make_unique<podio::ROOTWriter>("output.root", event_store_.get());

}

void PODIOWriterModule::run(Event* event) {
    auto pixel_messages = messenger_->fetchMultiMessage<PixelHitMessage>(this, event);
    

    auto evt = std::make_unique<LCEventImpl>(); // create the event
    evt->setRunNumber(1);
    evt->setEventNumber(static_cast<int>(event->number)); // set the event attributes
    evt->parameters().setValue("EventType", 2);

    auto output_col_vec = std::vector<LCCollectionVec*>();
    auto output_col_encoder_vec = std::vector<std::unique_ptr<CellIDEncoder<TrackerDataImpl>>>();
    // Prepare dynamic output setup and their CellIDEncoders which are defined by the user's config
    for(size_t i = 0; i < collection_names_vector_.size(); ++i) {
        output_col_vec.emplace_back(new LCCollectionVec(LCIO::TRACKERDATA));
        output_col_encoder_vec.emplace_back(
            std::make_unique<CellIDEncoder<TrackerDataImpl>>(eutelescope::gTrackerDataEncoding, output_col_vec.back()));
    }

    LCCollectionVec* mc_cluster_vec = nullptr;
    LCCollectionVec* mc_cluster_raw_vec = nullptr;
    LCCollectionVec* mc_hit_vec = nullptr;
    LCCollectionVec* mc_track_vec = nullptr;
    
    // Write the event to the output file
    writer_->writeEvent();
    event_store_->clear();

}

void PODIOWriterModule::finalize() {
    lcWriter_->close();
    // Print statistics
    LOG(STATUS) << "Wrote " << write_cnt_ << " events to file:" << std::endl << lcio_file_name_;

    // Write geometry:
    std::ofstream geometry_file;
    if(!geometry_file_name_.empty()) {
        geometry_file.open(geometry_file_name_, std::ios_base::out | std::ios_base::trunc);
        if(!geometry_file.good()) {
            throw ModuleError("Cannot write to GEAR geometry file");
        }

        auto detectors = geo_mgr_->getDetectors();
        geometry_file << "<?xml version=\"1.0\" encoding=\"utf-8\"?>" << std::endl
                      << "<!-- ?xml-stylesheet type=\"text/xsl\" href=\"https://cern.ch/allpix-squared/\"? -->" << std::endl
                      << "<gear>" << std::endl;

        geometry_file << "  <global detectorName=\"" << detector_name_ << "\"/>" << std::endl;
        if(geo_mgr_->getMagneticFieldType() == MagneticFieldType::CONSTANT) {
            ROOT::Math::XYZVector b_field = geo_mgr_->getMagneticField(ROOT::Math::XYZPoint(0., 0., 0.));
            geometry_file << "  <BField type=\"ConstantBField\" x=\"" << Units::convert(b_field.x(), "T") << "\" y=\""
                          << Units::convert(b_field.y(), "T") << "\" z=\"" << Units::convert(b_field.z(), "T") << "\"/>"
                          << std::endl;
        } else if(geo_mgr_->getMagneticFieldType() == MagneticFieldType::NONE) {
            geometry_file << "  <BField type=\"ConstantBField\" x=\"0.0\" y=\"0.0\" z=\"0.0\"/>" << std::endl;
        } else {
            LOG(WARNING) << "Field type not handled by GEAR geometry. Writing null magnetic field instead.";
            geometry_file << "  <BField type=\"ConstantBField\" x=\"0.0\" y=\"0.0\" z=\"0.0\"/>" << std::endl;
        }

        geometry_file << "  <detectors>" << std::endl;
        geometry_file << "    <detector name=\"SiPlanes\" geartype=\"SiPlanesParameters\">" << std::endl;
        geometry_file << "      <siplanesType type=\"TelescopeWithoutDUT\"/>" << std::endl;
        geometry_file << "      <siplanesNumber number=\"" << detectors.size() << "\"/>" << std::endl;
        geometry_file << "      <siplanesID ID=\"" << 0 << "\"/>" << std::endl;
        geometry_file << "      <layers>" << std::endl;

        for(auto& detector : detectors) {
            // Write header for the layer:
            geometry_file << "      <!-- Allpix Squared Detector: " << detector->getName()
                          << " - type: " << detector->getType() << " -->" << std::endl;
            geometry_file << "        <layer>" << std::endl;

            auto position = detector->getPosition();

            auto model = detector->getModel();
            auto npixels = model->getNPixels();
            auto pitch = model->getPixelSize();

            auto total_size = model->getSize();
            auto sensitive_size = model->getSensorSize();

            // Write ladder
            geometry_file << "          <ladder ID=\"" << detector_names_to_id_[detector->getName()] << "\"" << std::endl;
            geometry_file << "            positionX=\"" << Units::convert(position.x(), "mm") << "\"\tpositionY=\""
                          << Units::convert(position.y(), "mm") << "\"\tpositionZ=\"" << Units::convert(position.z(), "mm")
                          << "\"" << std::endl;

            auto angles = getRotationAnglesFromMatrix(detector->getOrientation());

            geometry_file << "            rotationZY=\"" << Units::convert(-angles[0], "deg") << "\"     rotationZX=\""
                          << Units::convert(-angles[1], "deg") << "\"   rotationXY=\"" << Units::convert(-angles[2], "deg")
                          << "\"" << std::endl;
            geometry_file << "            sizeX=\"" << Units::convert(total_size.x(), "mm") << "\"\tsizeY=\""
                          << Units::convert(total_size.y(), "mm") << "\"\tthickness=\""
                          << Units::convert(total_size.z(), "mm") << "\"" << std::endl;
            geometry_file << "            radLength=\"93.65\"" << std::endl;
            geometry_file << "            />" << std::endl;

            // Write sensitive
            geometry_file << "          <sensitive ID=\"" << detector_names_to_id_[detector->getName()] << "\"" << std::endl;
            geometry_file << "            positionX=\"" << Units::convert(position.x(), "mm") << "\"\tpositionY=\""
                          << Units::convert(position.y(), "mm") << "\"\tpositionZ=\"" << Units::convert(position.z(), "mm")
                          << "\"" << std::endl;
            geometry_file << "            sizeX=\"" << Units::convert(npixels.x() * pitch.x(), "mm") << "\"\tsizeY=\""
                          << Units::convert(npixels.y() * pitch.y(), "mm") << "\"\tthickness=\""
                          << Units::convert(sensitive_size.z(), "mm") << "\"" << std::endl;
            geometry_file << "            npixelX=\"" << npixels.x() << "\"\tnpixelY=\"" << npixels.y() << "\"" << std::endl;
            geometry_file << "            pitchX=\"" << Units::convert(pitch.x(), "mm") << "\"\tpitchY=\""
                          << Units::convert(pitch.y(), "mm") << "\"\tresolution=\""
                          << Units::convert(pitch.x() / std::sqrt(12), "mm") << "\"" << std::endl;
            geometry_file << "            rotation1=\"1.0\"\trotation2=\"0.0\"" << std::endl;
            geometry_file << "            rotation3=\"0.0\"\trotation4=\"1.0\"" << std::endl;
            geometry_file << "            radLength=\"93.65\"" << std::endl;
            geometry_file << "            />" << std::endl;

            // End the layer:
            geometry_file << "        </layer>" << std::endl;
        }

        // Close XML tree:
        geometry_file << "      </layers>" << std::endl
                      << "    </detector>" << std::endl
                      << "  </detectors>" << std::endl
                      << "</gear>" << std::endl;

        LOG(STATUS) << "Wrote GEAR geometry to file:" << std::endl << geometry_file_name_;
    }
}
