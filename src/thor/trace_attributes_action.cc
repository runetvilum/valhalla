#include <prime_server/prime_server.hpp>

using namespace prime_server;

#include <valhalla/baldr/json.h>
#include <valhalla/baldr/graphconstants.h>
#include <valhalla/baldr/directededge.h>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <valhalla/midgard/logging.h>
#include <valhalla/midgard/constants.h>
#include <valhalla/baldr/errorcode_util.h>
#include <valhalla/odin/util.h>
#include <valhalla/proto/tripdirections.pb.h>
#include <valhalla/proto/trippath.pb.h>

#include "thor/service.h"
#include "thor/trip_path_controller.h"

using namespace valhalla;
using namespace valhalla::midgard;
using namespace valhalla::baldr;
using namespace valhalla::sif;
using namespace valhalla::odin;
using namespace valhalla::thor;


namespace {
  const headers_t::value_type CORS { "Access-Control-Allow-Origin", "*" };
  const headers_t::value_type JSON_MIME { "Content-type", "application/json;charset=utf-8" };
  const headers_t::value_type JS_MIME { "Content-type", "application/javascript;charset=utf-8" };

  const std::unordered_map<int, std::string> vehicle_to_string {
    { static_cast<int>(TripPath_VehicleType_kCar), "car" },
    { static_cast<int>(TripPath_VehicleType_kMotorcycle), "motorcycle" },
    { static_cast<int>(TripPath_VehicleType_kAutoBus), "bus" },
    { static_cast<int>(TripPath_VehicleType_kTractorTrailer), "tractor_trailer" },
  };

  std::unordered_map<int, std::string> pedestrian_to_string {
    { static_cast<int>(TripPath_PedestrianType_kFoot), "foot" },
    { static_cast<int>(TripPath_PedestrianType_kWheelchair), "wheelchair" },
    { static_cast<int>(TripPath_PedestrianType_kSegway), "segway" },
  };

  std::unordered_map<int, std::string> bicycle_to_string {
    { static_cast<int>(TripPath_BicycleType_kRoad), "road" },
    { static_cast<int>(TripPath_BicycleType_kCross), "cross" },
    { static_cast<int>(TripPath_BicycleType_kHybrid), "hybrid" },
    { static_cast<int>(TripPath_BicycleType_kMountain), "mountain" },
  };

  std::pair<std::string, std::string> travel_mode_type(const TripPath::Edge& edge) {
    switch (edge.travel_mode()) {
      case TripPath_TravelMode_kDrive: {
        auto i = edge.has_vehicle_type() ? vehicle_to_string.find(edge.vehicle_type()) : vehicle_to_string.cend();
        return i == vehicle_to_string.cend() ? std::make_pair("drive", "car") : std::make_pair("drive", i->second);
      }
      case TripPath_TravelMode_kPedestrian: {
        auto i = edge.has_pedestrian_type() ? pedestrian_to_string.find(edge.pedestrian_type()) : pedestrian_to_string.cend();
        return i == pedestrian_to_string.cend() ? std::make_pair("pedestrian", "foot") : std::make_pair("pedestrian", i->second);
      }
      case TripPath_TravelMode_kBicycle: {
        auto i = edge.has_bicycle_type() ? bicycle_to_string.find(edge.bicycle_type()) : bicycle_to_string.cend();
        return i == bicycle_to_string.cend() ? std::make_pair("bicycle", "road") : std::make_pair("bicycle", i->second);
      }
    }
  }

  const std::unordered_map<uint8_t, std::string> SidewalkStrings = {
    {static_cast<uint8_t>(TripPath_Sidewalk_kNoSidewalk), "none"},
    {static_cast<uint8_t>(TripPath_Sidewalk_kLeft), "left"},
    {static_cast<uint8_t>(TripPath_Sidewalk_kRight), "right"},
    {static_cast<uint8_t>(TripPath_Sidewalk_kBothSides), "both"},
  };
  inline std::string to_string(TripPath_Sidewalk s) {
    auto i = SidewalkStrings.find(static_cast<uint8_t>(s));
    if(i == SidewalkStrings.cend())
      return "null";
    return i->second;
  }

  const std::unordered_map<uint8_t, std::string> TraversabilityStrings = {
    {static_cast<uint8_t>(TripPath_Traversability_kNone), "none"},
    {static_cast<uint8_t>(TripPath_Traversability_kForward), "forward"},
    {static_cast<uint8_t>(TripPath_Traversability_kBackward), "backward"},
    {static_cast<uint8_t>(TripPath_Traversability_kBoth), "both"},
  };
  inline std::string to_string(TripPath_Traversability t) {
    auto i = TraversabilityStrings.find(static_cast<uint8_t>(t));
    if(i == TraversabilityStrings.cend())
      return "null";
    return i->second;
  }

  json::MapPtr serialize(const TripPathController& controller,
                       const valhalla::odin::TripPath& trip_path,
                       const boost::optional<std::string>& id,
                       const DirectionsOptions& directions_options) {
    // Length and speed default to kilometers
    double scale = 1;
    if (directions_options.has_units()
        && directions_options.units() == DirectionsOptions::kMiles) {
      scale = kMilePerKm;
    }

    // Loop over edges to add attributes
    json::ArrayPtr edge_array = json::array({});
    for (int i = 1; i < trip_path.node().size(); i++) {

      if (trip_path.node(i-1).has_edge()) {
        const auto& edge = trip_path.node(i - 1).edge();

        // Process each edge
        auto edge_map = json::map({});
        if (edge.has_truck_route())
          edge_map->emplace("truck_route", static_cast<bool>(edge.truck_route()));
        if (edge.has_truck_speed() && (edge.truck_speed() > 0))
          edge_map->emplace("truck_speed", static_cast<uint64_t>(std::round(edge.truck_speed() * scale)));
        if (edge.has_speed_limit() && (edge.speed_limit() > 0))
          edge_map->emplace("speed_limit", static_cast<uint64_t>(std::round(edge.speed_limit() * scale)));
        if (edge.has_density())
          edge_map->emplace("density", static_cast<uint64_t>(edge.density()));
        if (edge.has_sidewalk())
          edge_map->emplace("sidewalk", to_string(edge.sidewalk()));
        if (edge.has_bicycle_network())
          edge_map->emplace("bicycle_network", static_cast<uint64_t>(edge.bicycle_network()));
        if (edge.has_cycle_lane())
          edge_map->emplace("cycle_lane", to_string(static_cast<CycleLane>(edge.cycle_lane())));
        if (edge.has_lane_count())
          edge_map->emplace("lane_count", static_cast<uint64_t>(edge.lane_count()));
        if (edge.has_max_downward_grade())
          edge_map->emplace("max_downward_grade", static_cast<int64_t>(edge.max_downward_grade()));
        if (edge.has_max_upward_grade())
          edge_map->emplace("max_upward_grade", static_cast<int64_t>(edge.max_upward_grade()));
        if (edge.has_weighted_grade())
          edge_map->emplace("weighted_grade", json::fp_t{edge.weighted_grade(), 3});
        if (edge.has_way_id())
          edge_map->emplace("way_id", static_cast<uint64_t>(edge.way_id()));
        if (edge.has_id())
          edge_map->emplace("id", static_cast<uint64_t>(edge.id()));
        if (edge.has_travel_mode()) {
          auto mode_type = travel_mode_type(edge);
          edge_map->emplace("travel_mode", mode_type.first);
        }
        if (edge.has_surface())
          edge_map->emplace("surface", to_string(static_cast<baldr::Surface>(edge.surface())));
        if (edge.has_drive_on_right())
          edge_map->emplace("drive_on_right", static_cast<bool>(edge.drive_on_right()));
        if (edge.has_internal_intersection())
          edge_map->emplace("internal_intersection", static_cast<bool>(edge.internal_intersection()));
        if (edge.has_roundabout())
          edge_map->emplace("roundabout", static_cast<bool>(edge.roundabout()));
        if (edge.has_bridge())
          edge_map->emplace("bridge", static_cast<bool>(edge.bridge()));
        if (edge.has_tunnel())
          edge_map->emplace("tunnel", static_cast<bool>(edge.tunnel()));
        if (edge.has_unpaved())
          edge_map->emplace("unpaved", static_cast<bool>(edge.unpaved()));
        if (edge.has_toll())
            edge_map->emplace("toll", static_cast<bool>(edge.toll()));
       if (edge.has_use())
          edge_map->emplace("use", to_string(static_cast<baldr::Use>(edge.use())));
        if (edge.has_traversability())
          edge_map->emplace("traversability", to_string(edge.traversability()));
        if (edge.has_end_shape_index())
          edge_map->emplace("end_shape_index", static_cast<uint64_t>(edge.end_shape_index()));
        if (edge.has_begin_shape_index())
          edge_map->emplace("begin_shape_index", static_cast<uint64_t>(edge.begin_shape_index()));
        if (edge.has_end_heading())
          edge_map->emplace("end_heading", static_cast<uint64_t>(edge.end_heading()));
        if (edge.has_begin_heading())
          edge_map->emplace("begin_heading", static_cast<uint64_t>(edge.begin_heading()));
        if (edge.has_road_class())
          edge_map->emplace("road_class", to_string(static_cast<baldr::RoadClass>(edge.road_class())));
        if (edge.has_speed())
          edge_map->emplace("speed", static_cast<uint64_t>(std::round(edge.speed() * scale)));
        if (edge.has_length())
          edge_map->emplace("length", json::fp_t{edge.length() * scale, 3});
        if (edge.name_size() > 0) {
          auto names_array = json::array({});
          for (const auto& name : edge.name())
            names_array->push_back(name);
          edge_map->emplace("names", names_array);
        }

        // Process edge sign
        if (edge.has_sign()) {
          auto sign_map = json::map({});

          // Populate exit number array
          if (edge.sign().exit_number_size() > 0) {
            auto exit_number_array = json::array({});
            for (const auto& exit_number : edge.sign().exit_number()) {
              exit_number_array->push_back(exit_number);
            }
            sign_map->emplace("exit_number", exit_number_array);
          }

          // Populate exit branch array
          if (edge.sign().exit_branch_size() > 0) {
            auto exit_branch_array = json::array({});
            for (const auto& exit_branch : edge.sign().exit_branch()) {
              exit_branch_array->push_back(exit_branch);
            }
            sign_map->emplace("exit_branch", exit_branch_array);
          }

          // Populate exit toward array
          if (edge.sign().exit_toward_size() > 0) {
            auto exit_toward_array = json::array({});
            for (const auto& exit_toward : edge.sign().exit_toward()) {
              exit_toward_array->push_back(exit_toward);
            }
            sign_map->emplace("exit_toward", exit_toward_array);
          }

          // Populate exit name array
          if (edge.sign().exit_name_size() > 0) {
            auto exit_name_array = json::array({});
            for (const auto& exit_name : edge.sign().exit_name()) {
              exit_name_array->push_back(exit_name);
            }
            sign_map->emplace("exit_name", exit_name_array);
          }

          edge_map->emplace("sign", sign_map);
        }

        // Process edge end node only if any node items are enabled
        if (controller.node_attribute_enabled()) {
          const auto& node = trip_path.node(i);
          auto end_node_map = json::map({});

          if (node.intersecting_edge_size() > 0) {
            auto intersecting_edge_array = json::array({});
            for (const auto& xedge : node.intersecting_edge()) {
              auto xedge_map = json::map({});
              xedge_map->emplace("walkability", to_string(xedge.walkability()));
              xedge_map->emplace("cyclability", to_string(xedge.cyclability()));
              xedge_map->emplace("driveability", to_string(xedge.driveability()));
              xedge_map->emplace("from_edge_name_consistency", static_cast<bool>(xedge.prev_name_consistency()));
              xedge_map->emplace("to_edge_name_consistency", static_cast<bool>(xedge.curr_name_consistency()));
              xedge_map->emplace("begin_heading", static_cast<uint64_t>(xedge.begin_heading()));

              intersecting_edge_array->emplace_back(xedge_map);
            }
            end_node_map->emplace("intersecting_edges", intersecting_edge_array);
          }

          if (node.has_elapsed_time())
            end_node_map->emplace("elapsed_time", static_cast<uint64_t>(node.elapsed_time()));
          if (node.has_admin_index())
            end_node_map->emplace("admin_index", static_cast<uint64_t>(node.admin_index()));
          if (node.has_type())
            end_node_map->emplace("type", to_string(static_cast<baldr::NodeType>(node.type())));
          if (node.has_fork())
            end_node_map->emplace("fork", static_cast<bool>(node.fork()));
          if (node.has_time_zone())
            end_node_map->emplace("time_zone", node.time_zone());

          // TODO transit info at node
          // kNodeTransitStopInfoType = "node.transit_stop_info.type";
          // kNodeTransitStopInfoOnestopId = "node.transit_stop_info.onestop_id";
          // kNodetransitStopInfoName = "node.transit_stop_info.name";
          // kNodeTransitStopInfoArrivalDateTime = "node.transit_stop_info.arrival_date_time";
          // kNodeTransitStopInfoDepartureDateTime = "node.transit_stop_info.departure_date_time";
          // kNodeTransitStopInfoIsParentStop = "node.transit_stop_info.is_parent_stop";
          // kNodeTransitStopInfoAssumedSchedule = "node.transit_stop_info.assumed_schedule";
          // kNodeTransitStopInfoLatLon = "node.transit_stop_info.lat_lon";

          edge_map->emplace("end_node", end_node_map);
        }

        // TODO - transit info on edge
        // kEdgeTransitType = "edge.transit_type";
        // kEdgeTransitRouteInfoOnestopId = "edge.transit_route_info.onestop_id";
        // kEdgeTransitRouteInfoBlockId = "edge.transit_route_info.block_id";
        // kEdgeTransitRouteInfoTripId = "edge.transit_route_info.trip_id";
        // kEdgeTransitRouteInfoShortName = "edge.transit_route_info.short_name";
        // kEdgeTransitRouteInfoLongName = "edge.transit_route_info.long_name";
        // kEdgeTransitRouteInfoHeadsign = "edge.transit_route_info.headsign";
        // kEdgeTransitRouteInfoColor = "edge.transit_route_info.color";
        // kEdgeTransitRouteInfoTextColor = "edge.transit_route_info.text_color";
        // kEdgeTransitRouteInfoDescription = "edge.transit_route_info.description";
        // kEdgeTransitRouteInfoOperatorOnestopId = "edge.transit_route_info.operator_onestop_id";
        // kEdgeTransitRouteInfoOperatorName = "edge.transit_route_info.operator_name";
        // kEdgeTransitRouteInfoOperatorUrl = "edge.transit_route_info.operator_url";

        edge_array->emplace_back(edge_map);
      }
    }

    auto json = json::map({
      {"edges", edge_array}
    });
    if (id)
      json->emplace("id", *id);
    if (trip_path.has_shape())
      json->emplace("shape", trip_path.shape());

    return json;
  }

}

namespace valhalla {
namespace thor {

void thor_worker_t::filter_attributes(const boost::property_tree::ptree& request, TripPathController& controller) {
  std::string filter_action = request.get("filters.action", "");

  if (filter_action.size() && filter_action == "include") {
    controller.disable_all();
    for (const auto& kv : request.get_child("filters.attributes"))
      controller.attributes.at(kv.second.get_value<std::string>()) = true;

  } else if (filter_action.size() && filter_action == "exclude") {
    controller.enable_all();
    for (const auto& kv : request.get_child("filters.attributes"))
      controller.attributes.at(kv.second.get_value<std::string>()) = false;

  } else {
    controller.enable_all();
  }
}

/*
 * The trace_attributes action takes a GPS trace or latitude, longitude positions
 * from a portion of an existing route and returns detailed attribution along the
 * portion of the route. This includes details for each section of road along the
 * path as well as any intersections along the path.
 */
worker_t::result_t thor_worker_t::trace_attributes(
    const boost::property_tree::ptree &request,
    const std::string &request_str, http_request_info_t& request_info) {
  //get time for start of request
  auto s = std::chrono::system_clock::now();

  // Parse request
  parse_locations(request);
  parse_shape(request);
  parse_costing(request);
  parse_trace_config(request);
  /*
   * A flag indicating whether the input shape is a GPS trace or exact points from a
   * prior route run against the Valhalla road network.  Knowing that the input is from
   * Valhalla will allow an efficient “edge-walking” algorithm rather than a more extensive
   * map-matching method. If true, this enforces to only use exact route match algorithm.
   */
  odin::TripPath trip_path;
  TripPathController controller;
  filter_attributes(request, controller);
  auto shape_match = STRING_TO_MATCH.find(request.get<std::string>("shape_match", "walk_or_snap"));
  if (shape_match == STRING_TO_MATCH.cend())
    throw valhalla_exception_t{400, 445};
  else {
    // If the exact points from a prior route that was run against the Valhalla road network,
    // then we can traverse the exact shape to form a path by using edge-walking algorithm
    switch (shape_match->second) {
      case EDGE_WALK:
        try {
          //TODO: remove after dev complete
          LOG_INFO("in " + shape_match->first);
          trip_path = route_match(controller);
        } catch (...) {
          LOG_INFO(shape_match->first + " algorithm failed to find exact route match.  Try using shape_match:'walk_or_snap' to fallback to map-matching algorithm");
          valhalla_exception_t{400, 443};
        }
        break;
      // If non-exact shape points are used, then we need to correct this shape by sending them
      // through the map-matching algorithm to snap the points to the correct shape
      case MAP_SNAP:
        try {
          //TODO: remove after dev complete
          LOG_INFO("in " + shape_match->first);
          trip_path = map_match(controller);
        } catch (...) {
          LOG_INFO(shape_match->first + " algorithm failed to snap the shape points to the correct shape.");
          valhalla_exception_t{400, 444};
        }
        break;
      //If we think that we have the exact shape but there ends up being no Valhalla route match, then
      // then we want to fallback to try and use meili map matching to match to local route network.
      //No shortcuts are used and detailed information at every intersection becomes available.
      case WALK_OR_SNAP:
        //TODO: remove after dev complete
        LOG_INFO("in " + shape_match->first);
        trip_path = route_match(controller);
        if (trip_path.node().size() == 0) {
          LOG_INFO(shape_match->first + " algorithm failed to find exact route match; Falling back to map_match...");
          try {
            trip_path = map_match(controller);
          } catch (...) {
            LOG_INFO(shape_match->first + " algorithm failed to snap the shape points to the correct shape.");
            valhalla_exception_t{400, 444};
          }
        }
        break;
      }
    }

  auto id = request.get_optional<std::string>("id");
  // Get the directions_options if they are in the request
  DirectionsOptions directions_options;
  auto options = request.get_child_optional("directions_options");
  if(options)
    directions_options = valhalla::odin::GetDirectionsOptions(*options);

  //serialize output to Thor
  json::MapPtr json;
  if (trip_path.node().size() > 0)
    json = serialize(controller, trip_path, id, directions_options);
  else throw valhalla_exception_t{400, 442};

  //jsonp callback if need be
  std::ostringstream stream;
  auto jsonp = request.get_optional<std::string>("jsonp");
  if (jsonp)
    stream << *jsonp << '(';
  stream << *json;
  if (jsonp)
    stream << ')';

  // Get processing time for thor
  auto e = std::chrono::system_clock::now();
  std::chrono::duration<float, std::milli> elapsed_time = e - s;
  // TODO determine what to log
  //log request if greater than X (ms)
  if (!request_info.spare && (elapsed_time.count() / correlated.size()) > long_request) {
    LOG_WARN("thor::trace_attributes elapsed time (ms)::"+ std::to_string(elapsed_time.count()));
    LOG_WARN("thor::trace_attributes exceeded threshold::"+ request_str);
    midgard::logging::Log("valhalla_thor_long_request_trace_attributes", " [ANALYTICS] ");
  }
  http_response_t response(200, "OK", stream.str(), headers_t{CORS, jsonp ? JS_MIME : JSON_MIME});
  response.from_info(request_info);
  worker_t::result_t result{false};
  result.messages.emplace_back(response.to_string());
  return result;
}
}
}
