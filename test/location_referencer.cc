#include "test.h"

#include "baldr/graphreader.h"
#include "baldr/location_referencer.h"
#include "loki/search.h"
#include "midgard/openlr.h"
#include "mjolnir/pbfgraphparser.h"
#include "mjolnir/graphbuilder.h"
#include "mjolnir/graphenhancer.h"
#include "mjolnir/graphtilebuilder.h"
#include "mjolnir/graphvalidator.h"
#include "baldr/location.h"
#include "sif/dynamiccost.h"
#include "sif/pedestriancost.h"
#include "thor/astar.h"


#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>

#include <iostream>
#include <string>
#include <sstream>

using namespace std;
using namespace valhalla::midgard::OpenLR;

// Maximum deviation from expected decoded value
constexpr double kPrecisionThreshold = 0.00001;

struct testfixture {
    std::string descriptor;
    double expectedFirstCoordinateLongitude;
    double expectedFirstCoordinateLatitude;
    double expectedFirstCoordinateBearing;

    double expectedLastCoordinateLongitude;
    double expectedLastCoordinateLatitude;
    double expectedLastCoordinateBearing;
};

boost::property_tree::ptree json_to_pt(const std::string& json) {
    std::stringstream ss; ss << json;
    boost::property_tree::ptree pt;
    boost::property_tree::read_json(ss, pt);
    return pt;
}

//fake config
const auto conf = json_to_pt(R"({
    "mjolnir":{"tile_dir":"test/data/utrecht_tiles", "concurrency": 1},
    "loki":{
      "actions":["locate","route","sources_to_targets","optimized_route","isochrone","trace_route","trace_attributes"],
      "logging":{"long_request": 100},
      "service_defaults":{"minimum_reachability": 50,"radius": 0}
    },
    "thor":{"logging":{"long_request": 110}},
    "skadi":{"actons":["height"],"logging":{"long_request": 5}},
    "meili":{"customizable": ["turn_penalty_factor","max_route_distance_factor","max_route_time_factor","search_radius"],
             "mode":"auto","grid":{"cache_size":100240,"size":500},
             "default":{"beta":3,"breakage_distance":2000,"geometry":false,"gps_accuracy":5.0,"interpolation_distance":10,
             "max_route_distance_factor":5,"max_route_time_factor":5,"max_search_radius":200,"route":true,
             "search_radius":15.0,"sigma_z":4.07,"turn_penalty_factor":200}},
    "service_limits": {
      "auto": {"max_distance": 5000000.0, "max_locations": 20,"max_matrix_distance": 400000.0,"max_matrix_locations": 50},
      "auto_shorter": {"max_distance": 5000000.0,"max_locations": 20,"max_matrix_distance": 400000.0,"max_matrix_locations": 50},
      "bicycle": {"max_distance": 500000.0,"max_locations": 50,"max_matrix_distance": 200000.0,"max_matrix_locations": 50},
      "bus": {"max_distance": 5000000.0,"max_locations": 50,"max_matrix_distance": 400000.0,"max_matrix_locations": 50},
      "hov": {"max_distance": 5000000.0,"max_locations": 20,"max_matrix_distance": 400000.0,"max_matrix_locations": 50},
      "isochrone": {"max_contours": 4,"max_distance": 25000.0,"max_locations": 1,"max_time": 120},
      "max_avoid_locations": 50,"max_radius": 200,"max_reachability": 100,
      "multimodal": {"max_distance": 500000.0,"max_locations": 50,"max_matrix_distance": 0.0,"max_matrix_locations": 0},
      "pedestrian": {"max_distance": 250000.0,"max_locations": 50,"max_matrix_distance": 200000.0,"max_matrix_locations": 50,"max_transit_walking_distance": 10000,"min_transit_walking_distance": 1},
      "skadi": {"max_shape": 750000,"min_resample": 10.0},
      "trace": {"max_distance": 200000.0,"max_gps_accuracy": 100.0,"max_search_radius": 100,"max_shape": 16000,"max_best_paths":4,"max_best_paths_shape":100},
      "transit": {"max_distance": 500000.0,"max_locations": 50,"max_matrix_distance": 200000.0,"max_matrix_locations": 50},
      "truck": {"max_distance": 5000000.0,"max_locations": 20,"max_matrix_distance": 400000.0,"max_matrix_locations": 50}
    }
  })");

const testfixture testfixtures [] = {
    { "CgOa9yUQACODBQEqAL4jEw==", 5.069987, 52.119130, 3, 5.072967, 52.121030, 19 },
    { "CwOiYCUMoBNWAv9P/+MSBg==", 5.110692, 52.100590, 2, 5.108922, 52.100300, 6 },
    { "CxWj2OogyxJBDhDSAvwSUL4=", 30.431259, -30.757352, 14, 30.474319, -30.749712, 16 }
};

namespace {

  void test_match_location_references() {

    using namespace valhalla;

    //setup and purge
    baldr::GraphReader graph_reader(conf.get_child("mjolnir"));
    for(const auto& level : baldr::TileHierarchy::levels()) {
      auto level_dir = graph_reader.tile_dir() + "/" + std::to_string(level.first);
      if(boost::filesystem::exists(level_dir) && !boost::filesystem::is_empty(level_dir)) {
        boost::filesystem::remove_all(level_dir);
      }
    }

    std::string ways_file = "test_ways_trivial.bin";
    std::string way_nodes_file = "test_way_nodes_trivial.bin";
    std::string access_file = "test_access_trivial.bin";
    std::string restriction_file = "test_complex_restrictions_trivial.bin";
    auto osmdata = mjolnir::PBFGraphParser::Parse(conf.get_child("mjolnir"), {"test/data/utrecht_netherlands.osm.pbf"},
                                         ways_file, way_nodes_file, access_file, restriction_file);
    // Build the graph using the OSMNodes and OSMWays from the parser
    mjolnir::GraphBuilder::Build(conf, osmdata, ways_file, way_nodes_file, restriction_file);
    // Enhance the local level of the graph. This adds information to the local
    // level that is usable across all levels (density, administrative
    // information (and country based attribution), edge transition logic, etc.
    mjolnir::GraphEnhancer::Enhance(conf, access_file);

    // Validate the graph and add information that cannot be added until
    // full graph is formed.
    mjolnir::GraphValidator::Validate(conf);

    baldr::LocationReferencer referencer(graph_reader);

    for (const auto &fixture: testfixtures) {

      auto locRef = TwoPointLinearReference::fromBase64(fixture.descriptor);

      auto path = referencer.match(locRef);
      std::cout << path.size() << std::endl;
      assert(path.size() > 0);
    }

  }
}

int main(void)
{
  test::suite suite("openlr");

  suite.test(TEST_CASE(test_match_location_references));

  return suite.tear_down();
}