import numpy as np
import numba.types
import pyarrow

from galois.loops import do_all, do_all_operator
from galois.property_graph import PropertyGraph
from galois.shmem import setActiveThreads

#QUESTIONS: what is the legal way to get value from certain index of an array in pyarrow?

#method that sums up the degrees of the current node's neighbors
def sum_neighbor_deg(graph, nid, result_dict, deg_array):

    sum_neighbor_degree = 0

    #for each edge connected to curr node: 
    for edge in graph.edges(nid):
        #get destination node
        dst = graph.get_edge_dst(edge)
        sum += deg_array[nid]
        steal = True
        
    avg_neighbor_degree: sum_neighbor_degree / deg_array[nid]
    result_dict[nid] = avg_neighbor_degree

##method that sums up the weighted degrees of the current node's neighbors
def w_sum_neighbor_degree(graph, nid, resultDict, deg_array)
    # create map that will hold the results key = node id and value = avg_neighbor_deg
    w_sum_neighbor_degree= 0
    #for edge connected to curr node: 
    for edge in graph.edges(nid):
        #get destination node 
        dst = graph.get_edge_dst(edge)

        w_sum_neighbor_degree += (deg_array[nid] * getEdgeData(edge))

    w_avg_neighbor_degree: w_sum_neighbor_degree() / deg_array[nid]
    result_dict[nid] = w_avg_neighbor_degree

#helper method that fills the result dictionary where key = nid and value = its average neighbor degree 
@do_all_operator()
def non_weighted_helper(graph, nid, deg_array):
    # create map that will hold the results key = node id and value = avg_neighbor_deg
    result_dict: dict()
    num_nodes = graph.num_nodes()
    #for each node in graph G
    do_all(
        range(num_nodes),

        sum_neighbor_deg(graph, nid, result_dict, deg_array),

        steal=True,
    )

    return resultDict

    
#helper method that fills the result dictionary where key = nid and value = its weighted average neighbor degree 
@do_all_operator()
def weighted_helper(graph, nid, deg_array)
    # create map that will hold the results key = node id and value = avg_neighbor_deg
    result_dict: dict()
    num_nodes = graph.num_nodes()
    #for each node in graph G 
    do_all(
        range(num_nodes),
        w_avg_neighbor_degree(nid, result_dict, deg_array),
        steal=True,
    )

    return resultDict


def average_neighbor_degree(graph: PropertyGraph, source, target, nodes, weight):
   
    calculate_degree(graph: PropertyGraph, weight_property = weight)

    if source = "in" and target = "in":
        deg_array = graph.get_node_property(inPop)
    elif: 
        deg_array = graph.get_node_property(outPop)

    if weight == 'None': 
        print() list(non_weighted_helper(graph: PropertyGraph, deg_array))
    else: 
        print() list(weighted_helper(graph: PropertyGraph), deg_array))
