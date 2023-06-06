import open3d as o3d
import numpy as np
import sys

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Please provide the path to the ply file as an argument.")
        sys.exit(1)
    
    # Loading point cloud
    print("Loading point cloud")
    ptCloud = o3d.io.read_point_cloud(sys.argv[1])

    # Normal estimation
    ptCloud.estimate_normals(
        search_param=o3d.geometry.KDTreeSearchParamHybrid(radius=0.1, max_nn=30))

    # Orientation of normal vector is consistent with tangent plane 
    ptCloud.orient_normals_consistent_tangent_plane(10)
       
    # Surface reconstruction by ball pivoting algorithm
    distances = ptCloud.compute_nearest_neighbor_distance()
    avg_dist = np.mean(distances)
    radius = 2*avg_dist   
    radii = [radius * 2]
    recMeshBPA = o3d.geometry.TriangleMesh.create_from_point_cloud_ball_pivoting(
            ptCloud, o3d.utility.DoubleVector(radii))
    
    # Save the mesh to a ply file
    output_path = "mesh.ply"
    if len(sys.argv) >= 3:
        output_path = sys.argv[2]
    o3d.io.write_triangle_mesh(output_path, recMeshBPA)
    
    print(f"Mesh saved to '{output_path}'")

