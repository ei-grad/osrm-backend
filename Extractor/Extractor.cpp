/*

Copyright (c) 2013, Project OSRM, Dennis Luxen, others
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list
of conditions and the following disclaimer.
Redistributions in binary form must reproduce the above copyright notice, this
list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "Extractor.h"

#include "ExtractionContainers.h"
#include "ExtractionNode.h"
#include "ExtractionWay.h"
#include "ExtractorCallbacks.h"
#include "ExtractorOptions.h"
#include "RestrictionParser.h"
#include "ScriptingEnvironment.h"

#include "../Util/GitDescription.h"
#include "../Util/IniFileUtil.h"
#include "../DataStructures/ConcurrentQueue.h"
#include "../Util/OSRMException.h"
#include "../Util/simple_logger.hpp"
#include "../Util/TimingUtil.h"
#include "../Util/make_unique.hpp"

#include "../typedefs.h"

#include <luabind/luabind.hpp>

#include <osmium/io/any_input.hpp>

#include <tbb/parallel_for.h>
#include <tbb/task_scheduler_init.h>

#include <variant/optional.hpp>

#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{

int lua_error_callback(lua_State *L) // This is so I can use my own function as an
// exception handler, pcall_log()
{
    luabind::object error_msg(luabind::from_stack(L, -1));
    std::ostringstream error_stream;
    error_stream << error_msg;
    throw OSRMException("ERROR occured in profile script:\n" + error_stream.str());
}
}

int Extractor::Run(int argc, char *argv[])
{
    ExtractorConfig extractor_config;

    try
    {
        LogPolicy::GetInstance().Unmute();
        TIMER_START(extracting);

        if (!ExtractorOptions::ParseArguments(argc, argv, extractor_config))
        {
            return 0;
        }
        ExtractorOptions::GenerateOutputFilesNames(extractor_config);

        if (1 > extractor_config.requested_num_threads)
        {
            SimpleLogger().Write(logWARNING) << "Number of threads must be 1 or larger";
            return 1;
        }

        if (!boost::filesystem::is_regular_file(extractor_config.input_path))
        {
            SimpleLogger().Write(logWARNING)
                << "Input file " << extractor_config.input_path.string() << " not found!";
            return 1;
        }

        if (!boost::filesystem::is_regular_file(extractor_config.profile_path))
        {
            SimpleLogger().Write(logWARNING) << "Profile " << extractor_config.profile_path.string()
                                             << " not found!";
            return 1;
        }

        const unsigned recommended_num_threads = tbb::task_scheduler_init::default_num_threads();

        SimpleLogger().Write() << "Input file: " << extractor_config.input_path.filename().string();
        SimpleLogger().Write() << "Profile: " << extractor_config.profile_path.filename().string();
        SimpleLogger().Write() << "Threads: " << extractor_config.requested_num_threads;
        // if (recommended_num_threads != extractor_config.requested_num_threads)
        // {
        //     SimpleLogger().Write(logWARNING) << "The recommended number of threads is "
        //                                      << recommended_num_threads
        //                                      << "! This setting may have performance side-effects.";
        // }

        auto number_of_threads = std::max(1,
            std::min(static_cast<int>(recommended_num_threads), static_cast<int>(extractor_config.requested_num_threads)) );

        tbb::task_scheduler_init init(number_of_threads);

        SimpleLogger().Write() << "requested_num_threads: " << extractor_config.requested_num_threads;
        SimpleLogger().Write() << "number_of_threads: " << number_of_threads;

        // setup scripting environment
        ScriptingEnvironment scripting_environment(extractor_config.profile_path.string().c_str());

        std::unordered_map<std::string, NodeID> string_map;
        ExtractionContainers extraction_containers;

        string_map[""] = 0;
        auto extractor_callbacks =
            osrm::make_unique<ExtractorCallbacks>(extraction_containers, string_map);

        osmium::io::File infile(extractor_config.input_path.string());
        osmium::io::Reader reader(infile);
        osmium::io::Header header = reader.header();

        unsigned number_of_nodes = 0;
        unsigned number_of_ways = 0;
        unsigned number_of_relations = 0;
        unsigned number_of_others = 0;

        SimpleLogger().Write() << "Parsing in progress..";
        TIMER_START(parsing);

        std::string generator = header.get("generator");
        if (generator.empty())
        {
            generator = "unknown tool";
        }
        SimpleLogger().Write() << "input file generated by " << generator;

        // write .timestamp data file
        std::string timestamp = header.get("osmosis_replication_timestamp");
        if (timestamp.empty())
        {
            timestamp = "n/a";
        }
        SimpleLogger().Write() << "timestamp: " << timestamp;

        boost::filesystem::ofstream timestamp_out(extractor_config.timestamp_file_name);
        timestamp_out.write(timestamp.c_str(), timestamp.length());
        timestamp_out.close();

        // lua_State *lua_state = scripting_environment.getLuaState();
        luabind::set_pcall_callback(&lua_error_callback);

        // initialize vectors holding parsed objects
        tbb::concurrent_vector<std::pair<std::size_t, ExtractionNode>> resulting_nodes;
        tbb::concurrent_vector<std::pair<std::size_t, ExtractionWay>> resulting_ways;
        tbb::concurrent_vector<mapbox::util::optional<InputRestrictionContainer>> resulting_restrictions;

        while (osmium::memory::Buffer buffer = reader.read())
        {
            // create a vector of iterators into the buffer
            std::vector<osmium::memory::Buffer::iterator> elements;
            osmium::memory::Buffer::iterator iter = std::begin(buffer);
            while(iter != std::end(buffer))
            {
                elements.push_back(iter);
                iter = std::next(iter);
            }

            // clear resulting vectors
            resulting_nodes.clear();
            resulting_ways.clear();
            resulting_restrictions.clear();

            // SimpleLogger().Write(logDEBUG) << "elements count: " << elements.size();

            // parse OSM entities in parallel, store in resulting vectors
            tbb::parallel_for(tbb::blocked_range<std::size_t>(0, elements.size()),
                [&](const tbb::blocked_range<std::size_t>& range)
                {
            for (auto x = range.begin(); x != range.end(); ++x)
            {
                auto entity = elements[x];

                ExtractionNode result_node;
                ExtractionWay result_way;
                // RestrictionParser restriction_parser(scripting_environment);

                switch (entity->type())
                {
                case osmium::item_type::node:
                    ++number_of_nodes;
                    result_node.Clear();
                    luabind::call_function<void>(scripting_environment.getLuaState(),
                                                 "node_function",
                                                 boost::cref(static_cast<osmium::Node &>(*entity)),
                                                 boost::ref(result_node));
                    resulting_nodes.emplace_back(x, result_node);
                    // extractor_callbacks->ProcessNode(static_cast<osmium::Node &>(*entity),
                    //                                  result_node);
                    break;
                case osmium::item_type::way:
                    ++number_of_ways;
                    result_way.Clear();
                    luabind::call_function<void>(scripting_environment.getLuaState(),
                                                 "way_function",
                                                 boost::cref(static_cast<osmium::Way &>(*entity)),
                                                 boost::ref(result_way));
                    resulting_ways.emplace_back(x, result_way);
                    // extractor_callbacks->ProcessWay(static_cast<osmium::Way &>(*entity), result_way);
                    break;
                case osmium::item_type::relation:
                    ++number_of_relations;
                    // resulting_restrictions.emplace_back(restriction_parser.TryParse(static_cast<osmium::Relation &>(*entity)));
                    // extractor_callbacks->ProcessRestriction(restriction_parser.TryParse(static_cast<osmium::Relation &>(*entity)));
                    break;
                default:
                    ++number_of_others;
                    break;
                }
            }
                            }
            );

            // put parsed objects thru extractor callbacks
            for (const auto &result : resulting_nodes)
            {
                extractor_callbacks->ProcessNode(static_cast<osmium::Node &>(*(elements[result.first])),
                                                 result.second);
            }
            for (const auto &result : resulting_ways)
            {
                extractor_callbacks->ProcessWay(static_cast<osmium::Way &>(*(elements[result.first])),
                                                 result.second);
            }
            for (const auto &result : resulting_restrictions)
            {
                extractor_callbacks->ProcessRestriction(result);
            }
        }
        TIMER_STOP(parsing);
        SimpleLogger().Write() << "Parsing finished after " << TIMER_SEC(parsing) << " seconds";
        SimpleLogger().Write() << "Raw input contains " << number_of_nodes << " nodes, "
                               << number_of_ways << " ways, and " << number_of_relations
                               << " relations";

        extractor_callbacks.reset();

        if (extraction_containers.all_edges_list.empty())
        {
            SimpleLogger().Write(logWARNING) << "The input data is empty, exiting.";
            return 1;
        }

        extraction_containers.PrepareData(extractor_config.output_file_name,
                                          extractor_config.restriction_file_name);

        TIMER_STOP(extracting);
        SimpleLogger().Write() << "extraction finished after " << TIMER_SEC(extracting) << "s";
        SimpleLogger().Write() << "To prepare the data for routing, run: "
                               << "./osrm-prepare " << extractor_config.output_file_name
                               << std::endl;
    }
    catch (std::exception &e)
    {
        SimpleLogger().Write(logWARNING) << e.what();
        return 1;
    }
    return 0;
}
