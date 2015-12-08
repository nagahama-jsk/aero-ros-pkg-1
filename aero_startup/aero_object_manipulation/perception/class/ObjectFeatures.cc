#include "aero_object_manipulation/perception/class/ObjectFeatures.hh"

using namespace aero;
using namespace perception;

//////////////////////////////////////////////////
ObjectFeatures::ObjectFeatures()
{
  status_ = 0;
  lost_count_ = lost_threshold_;
}

//////////////////////////////////////////////////
ObjectFeatures::~ObjectFeatures()
{
}

//////////////////////////////////////////////////
void ObjectFeatures::ExtractObjectFeatures(
    Eigen::Vector3f _center, std::vector<Eigen::Vector3f> _vertices,
    pcl::PointCloud<pcl::PointXYZ>::Ptr _cloud)
{
  if (_vertices.size() == 0) // object detection fail
  {
    // ROS_ERROR("no points detected");
    ++lost_count_;

    if (lost_count_ >= lost_threshold_) // Object does not exist
    {
      target_center_camera_ = Eigen::Vector3f(0, 0, 0);
      target_center_world_ = Eigen::Vector3f(0, 0, 0);
      target_pose_camera_ = Eigen::Quaternionf(1, 0, 0, 0);
      target_pose_world_left_ = Eigen::Quaternionf(1, 0, 0, 0);
      target_pose_world_right_ = Eigen::Quaternionf(1, 0, 0, 0);
      status_ = aero::status::aborted;
    }
    else // Object might exist but was not found for this frame
    {
      status_ = aero::status::warning;
    }

    this->BroadcastTf();
    return; // object detection failed
  }

  // an object moving faster than 0.5 m/frame, it cannot be detected
  // this is to cut outliers
  if (lost_count_ == 0 && (_center - target_center_camera_).norm() > 0.5)
  {
    this->BroadcastTf();
    return;
  }

  lost_count_ = 0;
  target_center_camera_ = _center;

  // Calculate Object Normal of each cloud point

  pcl::NormalEstimation<pcl::PointXYZ, pcl::Normal> ne;
  ne.setInputCloud(_cloud);

  pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(
      new pcl::search::KdTree<pcl::PointXYZ>());
  ne.setSearchMethod(tree);

  pcl::PointCloud<pcl::Normal>::Ptr normals(new pcl::PointCloud<pcl::Normal>);
  ne.setRadiusSearch(0.01);
  ne.compute(*normals);

  // Calculate Object Normal (average of point normals)

  int normal_count = 0;
  Eigen::Vector3f normal(0, 0, 0);
  for (unsigned int i = 0; i < normals->points.size(); ++i)
  {
    if (normals->points[i].normal_x != normals->points[i].normal_x ||
	normals->points[i].normal_y != normals->points[i].normal_y ||
	normals->points[i].normal_z != normals->points[i].normal_z)
      continue; // reject point with nan value normal
    ++normal_count;
    normal += Eigen::Vector3f(normals->points[i].normal_x,
			      normals->points[i].normal_y,
			      normals->points[i].normal_z);
  }

  if (normal_count == 0) // normal calculation fail
  {
    // ROS_ERROR("no valid normals");
    this->BroadcastTf();
    status_ = aero::status::warning;
    return; // normals were not computed correctly
  }

  normal = normal * (1.0 / normal_count);
  normal.normalize();

  Eigen::Vector3f transpose_normal = Eigen::Vector3f(1, 0, 0).cross(normal);
  float theta_normal  = asin(transpose_normal.norm());
  if (theta_normal != theta_normal) // check nan
  {
    // ROS_ERROR("error occured while calculation normal");
    this->BroadcastTf();
    status_ = aero::status::warning;
    return;
  }
  if ((Eigen::Vector3f(1, 0, 0)).dot(normal) < 0)
    theta_normal = M_PI - theta_normal;
  transpose_normal.normalize();

  // Calculate Object Axis (PCA)

  Eigen::MatrixXf m(_vertices.size(), 3);
  for (unsigned int i = 0; i < _vertices.size(); ++i)
  {
    m(i, 0) = _vertices[i][0] - _center[0];
    m(i, 1) = _vertices[i][1] - _center[1];
    m(i, 2) = _vertices[i][2] - _center[2];
  }
  Eigen::JacobiSVD<Eigen::MatrixXf> svd(
      (1.0 / _vertices.size()) * m.transpose() * m,
      Eigen::ComputeThinU | Eigen::ComputeThinV);
  Eigen::Vector3f axis = svd.matrixU().col(0);
  axis.normalize();

  Eigen::Vector3f transpose_axis = Eigen::Vector3f(1, 0, 0).cross(axis);
  float theta_axis = asin(transpose_axis.norm());
  if (theta_axis != theta_axis) // check nan
  {
    // ROS_ERROR("error occured while calculating axis");
    this->BroadcastTf();
    status_ = aero::status::warning;
    return;
  }
  if ((Eigen::Vector3f(1, 0, 0)).dot(axis) < 0) theta_axis = M_PI - theta_axis;
  transpose_axis.normalize();

  // Extract Object Normal Component Perpendicular to Object Axis

  Eigen::Quaternionf axis_pose_q(cos(theta_axis / 2),
				 transpose_axis[0] * sin(theta_axis / 2),
				 transpose_axis[1] * sin(theta_axis / 2),
				 transpose_axis[2] * sin(theta_axis / 2));

  Eigen::Vector3f normal_x_axis = normal.cross(axis);
  normal_x_axis.normalize();
  Eigen::Vector3f axis_z = axis_pose_q * Eigen::Vector3f(0, 0, 1);
  axis_z.normalize();
  float theta_z = asin((normal_x_axis.cross(axis_z)).norm()) + M_PI/2;
  if (normal_x_axis.dot(axis_z) < 0) theta_z = 2 * M_PI - theta_z;
  Eigen::Quaternionf transpose_z(cos(theta_z / 2), sin(theta_z / 2), 0, 0);
  Eigen::Quaternionf pose_q = axis_pose_q * transpose_z;

  target_pose_camera_ = pose_q;

  // Transform Object Pose to Grasping Coordinates

  static tf::TransformListener tl;

  ros::Time now = ros::Time::now();
  tl.waitForTransform("leg_base_link", "ps4eye_frame", now, ros::Duration(2.0));

  try
  {
    tl.lookupTransform("leg_base_link", "ps4eye_frame", now, base_to_eye_);
    Eigen::Quaternionf base_to_eye_q =
      Eigen::Quaternionf(base_to_eye_.getRotation().w(),
			 base_to_eye_.getRotation().x(),
			 base_to_eye_.getRotation().y(),
			 base_to_eye_.getRotation().z());
    Eigen::Quaternionf world_q = base_to_eye_q * pose_q;
    target_center_world_ =
      Eigen::Vector3f(base_to_eye_.getOrigin().x(),
		      base_to_eye_.getOrigin().y(),
		      base_to_eye_.getOrigin().z()) + base_to_eye_q * _center;
    // X : axis, Z : normal -> X : normal , Z : axis
    Eigen::Quaternionf grasp_q = // rotate on axis Y by M_PI/2
      world_q * Eigen::Quaternionf(0.707107, 0.0, 0.707107, 0.0);
    Eigen::Vector3f coordinate_sgn = grasp_q * Eigen::Vector3f(0, 0, 1);
    target_pose_world_left_ = grasp_q;
    target_pose_world_right_ = grasp_q;
    // reset coords to grasp-able direction if needed
    if (coordinate_sgn.dot(Eigen::Vector3f(-1, -1, 1)) <
	coordinate_sgn.dot(Eigen::Vector3f(1, 1, -1)))
      target_pose_world_left_ = grasp_q * Eigen::Quaternionf(0.0, 1.0, 0.0, 0.0);
    if (coordinate_sgn.dot(Eigen::Vector3f(-1, 1, 1)) <
	coordinate_sgn.dot(Eigen::Vector3f(1, -1, -1)))
      target_pose_world_right_ = grasp_q * Eigen::Quaternionf(0.0, 1.0, 0.0, 0.0);

    this->BroadcastTf();
    status_ = aero::status::success;
  }
  catch (std::exception e)
  {
    ROS_ERROR("failed tf listen");
    this->BroadcastTf();
    status_ = aero::status::warning;
  }

  // Export results (mainly for debug)

  // pcl::PCLPointCloud2 pcl_out;
  // sensor_msgs::PointCloud2 msg;
  // pcl::toPCLPointCloud2(*cloud, pcl_out);
  // pcl_conversions::fromPCL(pcl_out, msg);
  // msg.header.frame_id = "ps4eye_frame";
  // msg.header.stamp = ros::Time(0); // get possible recent
  // pcl_pub_.publish(msg);
};

//////////////////////////////////////////////////
void ObjectFeatures::BroadcastTf()
{
  static tf::TransformBroadcaster br;
  tf::Transform transform;
  transform.setOrigin(tf::Vector3(target_center_world_[0],
				  target_center_world_[1],
				  target_center_world_[2]));
  tf::Quaternion q_l(target_pose_world_left_.x(),
		     target_pose_world_left_.y(),
		     target_pose_world_left_.z(),
		     target_pose_world_left_.w());
  tf::Quaternion q_r(target_pose_world_right_.x(),
		     target_pose_world_right_.y(),
		     target_pose_world_right_.z(),
		     target_pose_world_right_.w());
  transform.setRotation(q_l);
  br.sendTransform(tf::StampedTransform(transform, ros::Time::now(),
					"leg_base_link", "object_l"));
  transform.setRotation(q_r);
  br.sendTransform(tf::StampedTransform(transform, ros::Time::now(),
					"leg_base_link", "object_r"));
}

//////////////////////////////////////////////////
int ObjectFeatures::GetStatus()
{
  return status_;
}

//////////////////////////////////////////////////
tf::StampedTransform ObjectFeatures::GetBaseToEye()
{
  return base_to_eye_;
}

//////////////////////////////////////////////////
Eigen::Vector3f ObjectFeatures::GetTargetCenterCamera()
{
  return target_center_camera_;
}
