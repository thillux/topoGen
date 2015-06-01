/*
 * Copyright (c) 2013-2014, Michael Grey and Markus Theil
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config/CMDArgs.hpp"
#include "config/Config.hpp"
#include "config/PredefinedValues.hpp"
#include "db/PopulationDensityReader.hpp"
#include "geo/GeometricHelpers.hpp"
#include "output/GraphOutput.hpp"
#include "output/JSONOutput.hpp"
#include "output/KMLWriter.hpp"
#include "topo/base_topo/BetaSkeletonFilter.hpp"
#include "topo/base_topo/DelaunayGraphCreator.hpp"
#include "topo/base_topo/NodeImporter.hpp"
#include "topo/base_topo/OPTICSFilter.hpp"
#include "topo/base_topo/PopulationDensityFilter.hpp"
#include "topo/Graph.hpp"
#include "topo/sim_topo/SimulationTopology.hpp"
#include "util/PopulationDensityLineCalculator.hpp"
#include <cassert>
#include <cmath>
#include <fstream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>
#include <json/json.h>

constexpr double EARTH_RADIUS_KM = 6371.000785;

int main(int argc, char** argv) {
    std::unique_ptr<Config> config(new Config);
    std::unique_ptr<CMDArgs> args(new CMDArgs(argc, argv));
    std::unique_ptr<NodeImporter> nodeImport(new NodeImporter);

    /*
      READ CITY POSITIONS ON EARTH SURFACE
    */

    if (config->get<bool>("debug.enable") == false) {
        nodeImport->importCities(args->getSeed());
    } else {
        nodeImport->importCitiesFromFile();
    }

    Locations_Ptr locations = nodeImport->getLocations();

    /*
     *  FILTER LOCATIONS WITH OPTICS
     */

    unsigned int neighbourCluster_minPts = config->get<unsigned int>("neighbourCluster.minPts");
    assert(neighbourCluster_minPts > 0);

    double neighbourCluster_maxClusterDistance = config->get<double>("neighbourCluster.maxClusterDistance");
    assert(neighbourCluster_maxClusterDistance > 0.0);

    double neighbourCluster_eps = neighbourCluster_maxClusterDistance / EARTH_RADIUS_KM;
    std::unique_ptr<OPTICSFilter> neighbourCluster_optics(
        new OPTICSFilter(locations, neighbourCluster_eps, neighbourCluster_minPts, 0.8 * neighbourCluster_eps));
    neighbourCluster_optics->filter(args->getSeed());

    unsigned int metropolisCluster_minPts = config->get<unsigned int>("metropolisCluster.minPts");
    assert(metropolisCluster_minPts > 0);

    double metropolisCluster_maxClusterDistance = config->get<double>("metropolisCluster.maxClusterDistance");
    assert(metropolisCluster_maxClusterDistance > 0.0);

    double metropolisCluster_eps = metropolisCluster_maxClusterDistance / EARTH_RADIUS_KM;
    std::unique_ptr<OPTICSFilter> metropolisCluster_optics(
        new OPTICSFilter(locations, metropolisCluster_eps, metropolisCluster_minPts, 0.8 * metropolisCluster_eps));
    metropolisCluster_optics->filter(args->getSeed());

    // add all nodes to kdtree for node merging
    nodeImport->importSeacableLandingPoints();
    nodeImport->importSubmarineCableEdgesWaypoints();

    // reset nodeIDs of all imported nodes, corresponding ids in lemon graphs go from 0 to numNodes-1
    int nodeId = 0;
    for (auto node : *locations) {
        node->setId(nodeId);
        ++nodeId;
    }

    /*
      CREATE DELAUNAY TRIANGULATION
    */

    DelaunayGraphCreator* delGen = new DelaunayGraphCreator(*locations);
    delGen->create();

    /*
      KML CONFIG
    */

    Config_Ptr kmlConfig(config->subConfig("kml_graph_output"));

    // http://www.colourlovers.com/palette/2757956/)
    std::string pincolor = kmlConfig->get<std::string>("pins.color");
    double pinAlpha = kmlConfig->get<double>("pins.alpha");
    std::string edgecolor = kmlConfig->get<std::string>("edges.color");
    double edgeAlpha = kmlConfig->get<double>("edges.alpha");
    std::string seacablecolor = kmlConfig->get<std::string>("seacable.color");
    double seacableAlpha = kmlConfig->get<double>("seacable.alpha");
    std::string seacablePinColor = kmlConfig->get<std::string>("seacablepins.color");
    double seacablePinAlpha = kmlConfig->get<double>("seacablepins.alpha");
    std::string delaunayFile = kmlConfig->get<std::string>("delaunayFile");
    std::string gabrielFile = kmlConfig->get<std::string>("gabrielFile");

    //  KML OUTPUT DELAUNAY

    if (args->kmlOutputEnabled()) {
        std::unique_ptr<KMLWriter> kmlw(new KMLWriter(delGen->getTopology()));
        kmlw->setEdgeColor(edgecolor, edgeAlpha);
        kmlw->setPinColor(pincolor, pinAlpha);
        kmlw->setSeacableColor(seacablecolor, seacableAlpha);
        kmlw->setSeacablePinColor(seacablePinColor, seacablePinAlpha);
        kmlw->createKML();
        kmlw->write(delaunayFile.c_str());
    }

    /*
      CREATE BETA SKELETON FROM DELAUNAY TRIANGULATION
    */

    BaseTopology_Ptr baseTopo = delGen->getTopology();
    std::unique_ptr<BetaSkeletonFilter> betaGraph(new BetaSkeletonFilter(baseTopo));
    betaGraph->filterBetaSkeletonEdges();

    /*
     APPLY DENSITY FILTER
    */
    const bool enableLengthFilter = config->get<bool>("lengthFilter.enable");
    if (enableLengthFilter == true) {
        PopulationDensityFilter_Ptr densFilter(new PopulationDensityFilter(baseTopo));
        densFilter->filterByLength();
    }

    /*
     IMPORT SUBMARINE CABLES
    */

    // debug
    std::vector<std::pair<double, double>> degNodesExclSubmarine = baseTopo->getHighestDegreeNodes(2, false);
    std::vector<std::pair<double, double>> degNodesExclSubmarineUSonly = baseTopo->getHighestDegreeNodes(2, true);

    nodeImport->importSubmarineCableEdges(baseTopo);

    // only use greatest component of baseTopo
    baseTopo->prune();

    /*
      KML OUTPUT BETA SKELETON
    */

    if (args->kmlOutputEnabled()) {
        std::unique_ptr<KMLWriter> kmlw(new KMLWriter(baseTopo));
        kmlw->setEdgeColor(edgecolor, edgeAlpha);
        kmlw->setPinColor(pincolor, pinAlpha);
        kmlw->setSeacableColor(seacablecolor, seacableAlpha);
        kmlw->setSeacablePinColor(seacablePinColor, seacablePinAlpha);
        kmlw->createKML();
        kmlw->write(gabrielFile.c_str());
    }

    /*
      GRAPH OUTPUT
    */

    if (args->graphOutputEnabled()) {
        Config_Ptr simpleGraphConfig(config->subConfig("simple_graph_output"));

        // write out nodes
        std::string nodeFileName = simpleGraphConfig->get<std::string>("nodeFile");
        std::ofstream nodeFile(nodeFileName.c_str());
        assert(nodeFile.good());
        std::string edgeFileName = simpleGraphConfig->get<std::string>("edgeFile");
        std::ofstream edgeFile(edgeFileName.c_str());
        assert(edgeFile.good());

        std::unique_ptr<GraphOutput> graphWriter(new GraphOutput(baseTopo, nodeFile, edgeFile));
        graphWriter->writeNodes();
        graphWriter->writeEdges();
    }

    /*
      DEAL WITH SIMULATION NODES
    */
    // create simulation topology
    SimulationTopology_Ptr simTopo(new SimulationTopology(baseTopo));

    if (args->simNodesJSONFile().length() > 0) {
        // read nodes from json file
        std::string filePath = args->simNodesJSONFile();
        std::ifstream jsonFile(filePath.c_str(), std::ifstream::binary);
        Json::Reader jsonReader;
        Json::Value root;
        bool parsed = jsonReader.parse(jsonFile, root);
        // check if parsing was successfull
        assert(parsed == true);

        // insert each node
        const Json::Value nodes = root["nodes"];
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
            Json::Value node = nodes[i];
            SimulationNode_Ptr simNode(
                new SimulationNode(node["id"].asInt(), node["latitude"].asDouble(), node["longitude"].asDouble()));
            simTopo->addNode(simNode);
        }

        jsonFile.close();
    }

    /*
      GRAPH OUTPUT (JSON)
    */
    if (args->jsonOutputEnabled()) {
        std::unique_ptr<JSONOutput> jsonWriter(new JSONOutput(baseTopo));
        jsonWriter->createJSON();

        std::string jsonFileNameCLI = args->jsonOutputFile();

        Config_Ptr jsonGraphConfig(config->subConfig("json_graph_output"));

        std::string jsonFileNameConfig = jsonGraphConfig->get<std::string>("filename");

        // json command line arg has precedence over json config parameter
        std::string jsonFileName;

        if (jsonFileNameCLI.length() > 0) {
            jsonFileName = jsonFileNameCLI;
        } else {
            jsonFileName = jsonFileNameConfig;
        }

        if (jsonGraphConfig->get<bool>("pretty_print")) {
            jsonWriter->writePretty(jsonFileName.c_str());
        } else {
            jsonWriter->write(jsonFileName.c_str());
        }
    }

    return EXIT_SUCCESS;
}