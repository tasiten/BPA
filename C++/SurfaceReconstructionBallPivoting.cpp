// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// Copyright (c) 2018-2023 www.open3d.org
// SPDX-License-Identifier: MIT
// ----------------------------------------------------------------------------

#include <Eigen/Dense>
#include <iostream>
#include <list>

#include "open3d/geometry/IntersectionTest.h"
#include "open3d/geometry/KDTreeFlann.h"
#include "open3d/geometry/PointCloud.h"
#include "open3d/geometry/TriangleMesh.h"
#include "open3d/utility/Logging.h"

namespace open3d {
namespace geometry {

class BallPivotingVertex;
class BallPivotingEdge;
class BallPivotingTriangle;

typedef BallPivotingVertex* BallPivotingVertexPtr;
typedef std::shared_ptr<BallPivotingEdge> BallPivotingEdgePtr;
typedef std::shared_ptr<BallPivotingTriangle> BallPivotingTrianglePtr;

class BallPivotingVertex {
public:
    enum Type { Orphan = 0, Front = 1, Inner = 2 };

    BallPivotingVertex(int idx,
                       const Eigen::Vector3d& point,
                       const Eigen::Vector3d& normal)
        : idx_(idx), point_(point), normal_(normal), type_(Orphan) {}

    void UpdateType();

public:
    int idx_;
    const Eigen::Vector3d& point_;
    const Eigen::Vector3d& normal_;
    std::unordered_set<BallPivotingEdgePtr> edges_;
    Type type_;
};

class BallPivotingEdge {
public:
    enum Type { Border = 0, Front = 1, Inner = 2 };

    BallPivotingEdge(BallPivotingVertexPtr source, BallPivotingVertexPtr target)
        : source_(source), target_(target), type_(Type::Front) {}

    void AddAdjacentTriangle(BallPivotingTrianglePtr triangle);
    BallPivotingVertexPtr GetOppositeVertex();

public:
    BallPivotingVertexPtr source_;
    BallPivotingVertexPtr target_;
    //エッジが接する二つの三角形(triangle0, triangle1)
    BallPivotingTrianglePtr triangle0_;
    BallPivotingTrianglePtr triangle1_;
    Type type_;
};

class BallPivotingfTriangle {
public:
    BallPivotingTriangle(BallPivotingVertexPtr vert0,
                         BallPivotingVertexPtr vert1,
                         BallPivotingVertexPtr vert2,
                         Eigen::Vector3d ball_center)
        : vert0_(vert0),
          vert1_(vert1),
          vert2_(vert2),
          ball_center_(ball_center) {}

public:
    BallPivotingVertexPtr vert0_;
    BallPivotingVertexPtr vert1_;
    BallPivotingVertexPtr vert2_;
    Eigen::Vector3d ball_center_;
};


//頂点のタイプを決める．↓タイプの種類と説明
//Orphan：この状態は、その頂点がまだメッシュの一部として使われていない（つまり、それを使用するエッジまたは面がない）場合に設定されます。これらの頂点は「孤立している」または「孤児」であると見なされ、新しい三角形を形成するための候補となります。
//Front：この状態は、その頂点がメッシュの「フロント」（つまり、現在のメッシュの境界）に属している場合に設定されます。これらの頂点は、新しい三角形を形成するための適切な場所で、次にどの頂点を接続すべきかを決定するのに役立つ情報を提供します。
//Inner：この状態は、その頂点がメッシュの「内部」に完全に含まれている（つまり、すでに完全に接続されている）場合に設定されます。これらの頂点はすでにメッシュ形成に完全に組み込まれており、これ以上の処理は必要ありません。
void BallPivotingVertex::UpdateType() {
    //頂点がどのエッジにも所属していない
    if (edges_.empty()) {
        type_ = Type::Orphan;
    } else {
        for (const BallPivotingEdgePtr& edge : edges_) {
            //頂点が所属するエッジのタイプがInnerではない場合
            if (edge->type_ != BallPivotingEdge::Type::Inner) {
                type_ = Type::Front;
                return;
            }
        }
        type_ = Type::Inner;
    }
}


//エッジ(BallPivotingEdge)に隣接する三角形を追加する.edge->AddAdjacentTriangle(triangle)のような形で使われる
//エッジがどの三角形と隣接しているかをエッジ側が(triangle0やtriangle1として)記録するための関数
//三角形ABCが出来た時点で辺AB,BC,CAは三角形ABCに隣接していると言える．なので辺ABのtriangle0は三角形ABCになる
//そこに点Dが加わり，三角形BCDが出来たとすると，辺BCは三角形ABCと三角形BCDと隣接していることになる．
//辺BCのtriangle0は三角形ABC，triangle1は三角形BCDとなる．
void BallPivotingEdge::AddAdjacentTriangle(BallPivotingTrianglePtr triangle) {
    //すでに引数の三角形が辺のtriangle0又はtriangle1でない場合
    if (triangle != triangle0_ && triangle != triangle1_) {
        //triangle0がまだ登録されていない場合
        if (triangle0_ == nullptr) {
            //ここでtriangle0を作成する．
            triangle0_ = triangle;
            type_ = Type::Front;
            // update orientation
            if (BallPivotingVertexPtr opp = GetOppositeVertex()) {
                Eigen::Vector3d tr_norm =
                        (target_->point_ - source_->point_)
                                .cross(opp->point_ - source_->point_);
                tr_norm /= tr_norm.norm();
                Eigen::Vector3d pt_norm =
                        source_->normal_ + target_->normal_ + opp->normal_;
                pt_norm /= pt_norm.norm();
                if (pt_norm.dot(tr_norm) < 0) {
                    std::swap(target_, source_);
                }
            } else {
                utility::LogError("GetOppositeVertex() returns nullptr.");
            }
        //triangle1がまだ登録されていない場合
        } else if (triangle1_ == nullptr) {
            triangle1_ = triangle;
            type_ = Type::Inner;
        } else {
            utility::LogDebug("!!! This case should not happen");
        }
    }
}

//現在のエッジに対して反対側の頂点を取得するための関数，triangle0からtargetでもsourceでもない頂点を取得する
BallPivotingVertexPtr BallPivotingEdge::GetOppositeVertex() {
    //隣接する三角形がある(登録されている)場合
    if (triangle0_ != nullptr) {
        if (triangle0_->vert0_->idx_ != source_->idx_ &&
            triangle0_->vert0_->idx_ != target_->idx_) {
            return triangle0_->vert0_;
        } else if (triangle0_->vert1_->idx_ != source_->idx_ &&
                   triangle0_->vert1_->idx_ != target_->idx_) {
            return triangle0_->vert1_;
        } else {
            return triangle0_->vert2_;
        }
    } else {
        return nullptr;
    }
}

class BallPivoting {
public:
    BallPivoting(const PointCloud& pcd)//コンストラクタ関数，インスタンスが生成されるだけで実行される関数
        : has_normals_(pcd.HasNormals()), kdtree_(pcd) {
        mesh_ = std::make_shared<TriangleMesh>();//make_shardはインスタンス生成関数
        mesh_->vertices_ = pcd.points_;
        mesh_->vertex_normals_ = pcd.normals_;
        mesh_->vertex_colors_ = pcd.colors_;
        for (size_t vidx = 0; vidx < pcd.points_.size(); ++vidx) {
            vertices.emplace_back(new BallPivotingVertex(static_cast<int>(vidx),
                                                         pcd.points_[vidx],
                                                         pcd.normals_[vidx]));
        }
    }

    virtual ~BallPivoting() {
        for (auto vert : vertices) {
            delete vert;
        }
    }

    //3頂点と球の半径と計算された球の中心座標が格納されるcenterを引数とし，
    //球の中心座標を計算して，計算できたかどうかをBool値で返す．
    bool ComputeBallCenter(int vidx1,
                           int vidx2,
                           int vidx3,
                           double radius,
                           Eigen::Vector3d& center) {
        //頂点を取得
        const Eigen::Vector3d& v1 = vertices[vidx1]->point_;
        const Eigen::Vector3d& v2 = vertices[vidx2]->point_;
        const Eigen::Vector3d& v3 = vertices[vidx3]->point_;
        //頂点間の距離の二乗を計算する．
        double c = (v2 - v1).squaredNorm();
        double b = (v1 - v3).squaredNorm();
        double a = (v3 - v2).squaredNorm();

        //各射影係数を計算
        //射影係数とはベクトルを他のベクトルに沿って投影したときの長さや比率を表すために使われる係数
        //このコードにおいてはそれぞれの頂点が三角形の中心に対して持つ「重み」を表す
        //この値から外接円の中心座標を求められる
        double alpha = a * (b + c - a);
        double beta = b * (a + c - b);
        double gamma = c * (a + b - c);
        double abg = alpha + beta + gamma;

        //射影係数の合計値が0に近い場合は終了
        if (abg < 1e-16) {
            return false;
        }

        //射影係数を正規化
        alpha = alpha / abg;
        beta = beta / abg;
        gamma = gamma / abg;

        //各頂点の座標に射影係数をかけて，合計すると外接円の中心座標がでる．
        Eigen::Vector3d circ_center = alpha * v1 + beta * v2 + gamma * v3;
        //ヘロンの公式から頂点間の距離の積から外接円の半径の二乗を求める
        double circ_radius2 = a * b * c;//ここではまだ半径の二乗ではない
        a = std::sqrt(a);
        b = std::sqrt(b);
        c = std::sqrt(c);
        circ_radius2 = circ_radius2 /
                       ((a + b + c) * (b + c - a) * (c + a - b) * (a + b - c));

        //三角形から球の中心までの高さを求めている．
        //球の半径の二乗から外接円の半径の二乗を引くと高さの二乗が求められる．
        //ピタゴラスの定理を使っている．
        double height = radius * radius - circ_radius2;

        //高さが負の正の値の場合，球の中心座標を求めている
        if (height >= 0.0) {
            //法線計算
            Eigen::Vector3d tr_norm = (v2 - v1).cross(v3 - v1);//(v2 - v1)と(v3 - v1)の外積を計算する
            tr_norm /= tr_norm.norm();//法線ベクトルの正規化，.norm()はベクトルの長さを求める
            //各頂点の法線ベクトルを足す．
            Eigen::Vector3d pt_norm = vertices[vidx1]->normal_ +
                                      vertices[vidx2]->normal_ +
                                      vertices[vidx3]->normal_;
            pt_norm /= pt_norm.norm();//各頂点の法線ベクトルの合計を正規化する．つまり法線ベクトルの平均値を取る事に相当する．
            
            //法線ベクトルの反転
            if (tr_norm.dot(pt_norm) < 0) {
                tr_norm *= -1;
            }

            height = sqrt(height);//高さを求める(ルート)
            center = circ_center + height * tr_norm;//中心座標をcenterに格納
            return true;
        }
        return false;
    }

    //与えられた頂点から辺を生成
    BallPivotingEdgePtr GetLinkingEdge(const BallPivotingVertexPtr& v0,
                                       const BallPivotingVertexPtr& v1) {
        for (BallPivotingEdgePtr edge0 : v0->edges_) {
            for (BallPivotingEdgePtr edge1 : v1->edges_) {
                if (edge0->source_->idx_ == edge1->source_->idx_ &&
                    edge0->target_->idx_ == edge1->target_->idx_) {
                    return edge0;
                }
            }
        }
        return nullptr;
    }

    //与えられた3点から3次元メッシュを生成
    void CreateTriangle(const BallPivotingVertexPtr& v0,
                        const BallPivotingVertexPtr& v1,
                        const BallPivotingVertexPtr& v2,
                        const Eigen::Vector3d& center) {
        utility::LogDebug(
                "[CreateTriangle] with v0.idx={}, v1.idx={}, v2.idx={}",
                v0->idx_, v1->idx_, v2->idx_);
        BallPivotingTrianglePtr triangle =
                std::make_shared<BallPivotingTriangle>(v0, v1, v2, center);//新しいインスタンスを生成

        BallPivotingEdgePtr e0 = GetLinkingEdge(v0, v1);//エッジ生成
        if (e0 == nullptr) {
            e0 = std::make_shared<BallPivotingEdge>(v0, v1);
        }
        //エッジを三角形に登録する．triangle0やtraingle1を生成してエッジ側に記録させる．
        e0->AddAdjacentTriangle(triangle);
        v0->edges_.insert(e0);
        v1->edges_.insert(e0);

        BallPivotingEdgePtr e1 = GetLinkingEdge(v1, v2);//エッジ生成
        if (e1 == nullptr) {
            e1 = std::make_shared<BallPivotingEdge>(v1, v2);
        }
        //エッジを三角形に登録する．triangle0やtraingle1を生成してエッジ側に記録させる．
        e1->AddAdjacentTriangle(triangle);
        v1->edges_.insert(e1);
        v2->edges_.insert(e1);

        BallPivotingEdgePtr e2 = GetLinkingEdge(v2, v0);//エッジ生成
        if (e2 == nullptr) {
            e2 = std::make_shared<BallPivotingEdge>(v2, v0);
        }
        //エッジを三角形に登録する．triangle0やtraingle1を生成してエッジ側に記録させる．
        e2->AddAdjacentTriangle(triangle);
        v2->edges_.insert(e2);
        v0->edges_.insert(e2);

        //頂点のタイプ更新
        v0->UpdateType();
        v1->UpdateType();
        v2->UpdateType();

        Eigen::Vector3d face_normal =
                ComputeFaceNormal(v0->point_, v1->point_, v2->point_);//面の法線ベクトルを求める
        //計算した面法線と頂点法線がある程度同じ向きにするための処理，頂点の追加順で三角形の法線向きが変わる
        if (face_normal.dot(v0->normal_) > -1e-16) {//面の法線と頂点v0の法線が同じ方向を向いている場合
            mesh_->triangles_.emplace_back(
                    Eigen::Vector3i(v0->idx_, v1->idx_, v2->idx_));//新しい三角形を追加
        } else {//面の法線と頂点v0の法線が同じ方向を向いていない場合
            mesh_->triangles_.emplace_back(
                    Eigen::Vector3i(v0->idx_, v2->idx_, v1->idx_));//新しい三角形を追加
        }
        mesh_->triangle_normals_.push_back(face_normal);//法線を追加
    }

    //面の法線ベクトルを外積から求める
    Eigen::Vector3d ComputeFaceNormal(const Eigen::Vector3d& v0,
                                      const Eigen::Vector3d& v1,
                                      const Eigen::Vector3d& v2) {
        Eigen::Vector3d normal = (v1 - v0).cross(v2 - v0);
        double norm = normal.norm();
        if (norm > 0) {
            normal /= norm;
        }
        return normal;
    }

    //引数の3頂点が互いに接続可能かを判定する
    bool IsCompatible(const BallPivotingVertexPtr& v0,
                      const BallPivotingVertexPtr& v1,
                      const BallPivotingVertexPtr& v2) {
        utility::LogDebug("[IsCompatible] v0.idx={}, v1.idx={}, v2.idx={}",
                          v0->idx_, v1->idx_, v2->idx_);
        Eigen::Vector3d normal =
                ComputeFaceNormal(v0->point_, v1->point_, v2->point_);//面の法線計算
        //点の法線と面の法線の内積を計算して，負の値なら面の法線を逆の向きにする(閾値より小さいなら反転させる)．
        //内積の結果が正の値の場合は，二つのベクトルは同じ方向(似た方向)を向いているという事になる．
        if (normal.dot(v0->normal_) < -1e-16) {
            normal *= -1;
        }
        //3点全ての法線と面の法線の内積を計算し，3点と同じ方向(似た方向)を向いている場合はretはTrueになる．
        bool ret = normal.dot(v0->normal_) > -1e-16 &&
                   normal.dot(v1->normal_) > -1e-16 &&
                   normal.dot(v2->normal_) > -1e-16;
        utility::LogDebug("[IsCompatible] returns = {}", ret);
        return ret;
    }

    BallPivotingVertexPtr FindCandidateVertex(
            const BallPivotingEdgePtr& edge,
            double radius,
            Eigen::Vector3d& candidate_center) {
        utility::LogDebug("[FindCandidateVertex] edge=({}, {}), radius={}",
                          edge->source_->idx_, edge->target_->idx_, radius);
        //引数のエッジを構成する頂点を取得する
        BallPivotingVertexPtr src = edge->source_;
        BallPivotingVertexPtr tgt = edge->target_;

        const BallPivotingVertexPtr opp = edge->GetOppositeVertex();//三つ目の点(opp)を見つける，srcとtgtが含まれた三角形のもう一つの頂点を取得する
        if (opp == nullptr) {
            utility::LogError("edge->GetOppositeVertex() returns nullptr.");
            assert(opp == nullptr);
        }
        utility::LogDebug("[FindCandidateVertex] edge=({}, {}), opp={}",
                          src->idx_, tgt->idx_, opp->idx_);
        utility::LogDebug("[FindCandidateVertex] src={} => {}", src->idx_,
                          src->point_.transpose());
        utility::LogDebug("[FindCandidateVertex] tgt={} => {}", tgt->idx_,
                          tgt->point_.transpose());
        utility::LogDebug("[FindCandidateVertex] src={} => {}", opp->idx_,
                          opp->point_.transpose());

        Eigen::Vector3d mp = 0.5 * (src->point_ + tgt->point_);//二つのベクトルの中点(平均)を求める．point_はベクトルを表す
        utility::LogDebug("[FindCandidateVertex] edge=({}, {}), mp={}",
                          edge->source_->idx_, edge->target_->idx_,
                          mp.transpose());

        BallPivotingTrianglePtr triangle = edge->triangle0_;//引数のエッジが所属している三角形を取得
        const Eigen::Vector3d& center = triangle->ball_center_;//取得した三角形から球の中心ベクトルを取得する
        utility::LogDebug("[FindCandidateVertex] edge=({}, {}), center={}",
                          edge->source_->idx_, edge->target_->idx_,
                          center.transpose());

        Eigen::Vector3d v = tgt->point_ - src->point_;//二つのベクトルの差分を求める，つまりsrcからtgtへの方向ベクトル
        v /= v.norm();//方向ベクトルを正規化する．つまり方向ベクトルの大きさを計算し，単位ベクトルにする．

        Eigen::Vector3d a = center - mp;//中心ベクトルcneterから中点ベクトルmpへの方向ベクトル
        a /= a.norm();////方向ベクトルを正規化する．つまり方向ベクトルの大きさを計算し，単位ベクトルにする．

        //最近傍探索の結果を格納するための配列を準備
        std::vector<int> indices;
        std::vector<double> dists2;
        kdtree_.SearchRadius(mp, 2 * radius, indices, dists2);//mpを中心とした半径2*radiusの範囲内にある点を探索する．探索結果として範囲内点インデックスを配列indices，各点までの距離の2乗がdists2に格納される．
        utility::LogDebug("[FindCandidateVertex] found {} potential candidates",
                          indices.size());

        BallPivotingVertexPtr min_candidate = nullptr;
        double min_angle = 2 * M_PI;//2πを準備
        //探索した点をループで調べる
        for (auto nbidx : indices) {
            utility::LogDebug("[FindCandidateVertex] nbidx {:d}", nbidx);
            const BallPivotingVertexPtr& candidate = vertices[nbidx];//探索点を取得
            //点がsrcでもtgtでもoppでもないかを調べる．一致したらcontinueする
            if (candidate->idx_ == src->idx_ || candidate->idx_ == tgt->idx_ ||
                candidate->idx_ == opp->idx_) {
                utility::LogDebug(
                        "[FindCandidateVertex] candidate {:d} is a triangle "
                        "vertex of the edge",
                        candidate->idx_);
                continue;
            }
            utility::LogDebug("[FindCandidateVertex] candidate={:d} => {}",
                              candidate->idx_, candidate->point_.transpose());

            bool coplanar = IntersectionTest::PointsCoplanar(
                    src->point_, tgt->point_, opp->point_, candidate->point_);//引数の4点が同一平面上に存在するか．存在する場合はTrueを返す
            //各線分の最短距離が閾値未満か(つまり新たに生成される三角形が既存の三角形と交差市中を判定)，各点が同一平面上にあるかを判定，その場合はcontinue
            if (coplanar && (IntersectionTest::LineSegmentsMinimumDistance(
                                     mp, candidate->point_, src->point_,
                                     opp->point_) < 1e-12 ||
                             IntersectionTest::LineSegmentsMinimumDistance(
                                     mp, candidate->point_, tgt->point_,
                                     opp->point_) < 1e-12)) {
                utility::LogDebug(
                        "[FindCandidateVertex] candidate {:d} is intersecting "
                        "the existing triangle",
                        candidate->idx_);
                continue;
            }

            Eigen::Vector3d new_center;
            //srcとtgtとcandidateの球の中心座標を取得出来たかを判定，また新しい球の中心座標(new_center)を計算する
            if (!ComputeBallCenter(src->idx_, tgt->idx_, candidate->idx_,
                                   radius, new_center)) {
                utility::LogDebug(
                        "[FindCandidateVertex] candidate {:d} can not compute "
                        "ball",
                        candidate->idx_);
                continue;
            }
            utility::LogDebug("[FindCandidateVertex] candidate {:d} center={}",
                              candidate->idx_, new_center.transpose());

            
            //候補となる頂点candidateに対して、方向ベクトルbとそのベクトルとの角度（コサイン値）を計算する
            Eigen::Vector3d b = new_center - mp;//二つのベクトルの差分を求める，つまりnew_centerからmpへの方向ベクトル
            b /= b.norm();//方向ベクトルを正規化する．つまり方向ベクトルの大きさを計算し，単位ベクトルにする．
            utility::LogDebug(
                    "[FindCandidateVertex] candidate {:d} v={}, a={}, b={}",
                    candidate->idx_, v.transpose(), a.transpose(),
                    b.transpose());

            //これらはaとbの角度を計算するためにある．aは旧球と回転軸となっているエッジの中心(mp)のベクトルを表し，bは新球と回転軸となっているエッジの中心(mp)のベクトルを表している．
            //なのでエッジの中心を軸として，新旧の球の中心の角度を求めようとしている．
            //そして旧球と一番小さい角度で新たな点を見つけられる新球を見つけることが目的である．
            double cosinus = a.dot(b);
            cosinus = std::min(cosinus, 1.0);
            cosinus = std::max(cosinus, -1.0);
            utility::LogDebug(
                    "[FindCandidateVertex] candidate {:d} cosinus={:f}",
                    candidate->idx_, cosinus);

            double angle = std::acos(cosinus);//逆余弦を計算し、角度を求る

            Eigen::Vector3d c = a.cross(b);//aとbの外積を求める，aとbに垂直なベクトルを求める
            //vとcが反対を向いている場合
            if (c.dot(v) < 0) {
                angle = 2 * M_PI - angle;//計算された角度 angle を0~2πの範囲に修正
            }

            //angleがmin_angle以上の場合
            if (angle >= min_angle) {
                utility::LogDebug(
                        "[FindCandidateVertex] candidate {:d} angle {:f} > "
                        "min_angle {:f}",
                        candidate->idx_, angle, min_angle);
                continue;
            }

            bool empty_ball = true;
            //範囲内の点をループで調べる
            for (auto nbidx2 : indices) {
                const BallPivotingVertexPtr& nb = vertices[nbidx2];
                //範囲内点がsrc,tgt,condidateである場合，continue
                if (nb->idx_ == src->idx_ || nb->idx_ == tgt->idx_ ||
                    nb->idx_ == candidate->idx_) {
                    continue;
                }
                //範囲内点と新しい球の距離が一定範囲未満の場合
                if ((new_center - nb->point_).norm() < radius - 1e-16) {
                    utility::LogDebug(
                            "[FindCandidateVertex] candidate {:d} not an empty "
                            "ball",
                            candidate->idx_);
                    empty_ball = false;
                    break;
                }
            }

            //一度でも範囲内点と新しい球の距離が一定範囲未満だった場合，変数を更新する
            if (empty_ball) {
                utility::LogDebug("[FindCandidateVertex] candidate {:d} works",
                                  candidate->idx_);
                min_angle = angle;
                min_candidate = vertices[nbidx];
                candidate_center = new_center;
            }
        }

        if (min_candidate == nullptr) {
            utility::LogDebug("[FindCandidateVertex] returns nullptr");
        } else {
            utility::LogDebug("[FindCandidateVertex] returns {:d}",
                              min_candidate->idx_);
        }
        return min_candidate;//頂点を返す
    }

    //トライアングルメッシュを拡張する
    void ExpandTriangulation(double radius) {
        utility::LogDebug("[ExpandTriangulation] radius={}", radius);

        //Frontエッジがなくなるまでループ
        while (!edge_front_.empty()) {
            BallPivotingEdgePtr edge = edge_front_.front();//Frontエッジリストの先頭からFrontエッジを取り出す
            edge_front_.pop_front();//取り出したFrontエッジをリストから削除
            //取り出したエッジがFrontエッジではない場合
            if (edge->type_ != BallPivotingEdge::Front) {
                continue;
            }

            Eigen::Vector3d center;
            //Frontエッジから候補点を見つける
            BallPivotingVertexPtr candidate =
                    FindCandidateVertex(edge, radius, center);
            if (candidate == nullptr ||
                candidate->type_ == BallPivotingVertex::Type::Inner ||
                !IsCompatible(candidate, edge->source_, edge->target_)) {
                edge->type_ = BallPivotingEdge::Type::Border;
                border_edges_.push_back(edge);
                continue;
            }

            BallPivotingEdgePtr e0 = GetLinkingEdge(candidate, edge->source_);
            BallPivotingEdgePtr e1 = GetLinkingEdge(candidate, edge->target_);
            if ((e0 != nullptr && e0->type_ != BallPivotingEdge::Type::Front) ||
                (e1 != nullptr && e1->type_ != BallPivotingEdge::Type::Front)) {
                edge->type_ = BallPivotingEdge::Type::Border;
                border_edges_.push_back(edge);
                continue;
            }

            CreateTriangle(edge->source_, edge->target_, candidate, center);

            e0 = GetLinkingEdge(candidate, edge->source_);
            e1 = GetLinkingEdge(candidate, edge->target_);
            if (e0->type_ == BallPivotingEdge::Type::Front) {
                edge_front_.push_front(e0);
            }
            if (e1->type_ == BallPivotingEdge::Type::Front) {
                edge_front_.push_front(e1);
            }
        }
    }

    //引数の3頂点が三角形になれるかを判定する，また球の中心座標も計算する
    bool TryTriangleSeed(const BallPivotingVertexPtr& v0,
                         const BallPivotingVertexPtr& v1,
                         const BallPivotingVertexPtr& v2,
                         const std::vector<int>& nb_indices,
                         double radius,
                         Eigen::Vector3d& center) {
        utility::LogDebug(
                "[TryTriangleSeed] v0.idx={}, v1.idx={}, v2.idx={}, "
                "radius={}",
                v0->idx_, v1->idx_, v2->idx_, radius);

        //3頂点が接続可能か判定
        if (!IsCompatible(v0, v1, v2)) {
            return false;
        }

        BallPivotingEdgePtr e0 = GetLinkingEdge(v0, v2);//v0とv2から辺e0を生成
        BallPivotingEdgePtr e1 = GetLinkingEdge(v1, v2);//v1とv2から辺e1を生成
        //e0が存在し，e0のタイプがInnerの場合
        if (e0 != nullptr && e0->type_ == BallPivotingEdge::Type::Inner) {
            utility::LogDebug(
                    "[TryTriangleSeed] returns {} because e0 is inner edge",
                    false);
            return false;
        }
        //e1が存在し，e1のタイプがInnerの場合
        if (e1 != nullptr && e1->type_ == BallPivotingEdge::Type::Inner) {
            utility::LogDebug(
                    "[TryTriangleSeed] returns {} because e1 is inner edge",
                    false);
            return false;
        }

        //3頂点に接している球の中心座標を計算し，計算できたかのBool値を返す．
        //計算でき無かった場合はここで終了する．
        if (!ComputeBallCenter(v0->idx_, v1->idx_, v2->idx_, radius, center)) {
            utility::LogDebug(
                    "[TryTriangleSeed] returns {} could not compute ball "
                    "center",
                    false);
            return false;
        }

        // test if no other point is within the ball(ボール内に他の点が存在しないかをテストする)
        //近傍の頂点をループで順番に調べる
        for (const auto& nbidx : nb_indices) {
            const BallPivotingVertexPtr& v = vertices[nbidx];
            //引数の3頂点と調べている頂点が同じ場合は次の点を調べる
            if (v->idx_ == v0->idx_ || v->idx_ == v1->idx_ ||
                v->idx_ == v2->idx_) {
                continue;
            }
            //球の中心と頂点の距離を計算して，半径未満であれば球内にボールが存在するとみなして終了
            if ((center - v->point_).norm() < radius - 1e-16) {
                utility::LogDebug(
                        "[TryTriangleSeed] returns {} computed ball is not "
                        "empty",
                        false);
                return false;
            }
        }

        utility::LogDebug("[TryTriangleSeed] returns {}", true);
        return true;
    }

    //頂点と半径を引数とし，一番最初の三角形(シード三角形)の辺を見つけようとする
    //具体的な内容としてはフロントエッジを生成する．
    bool TrySeed(BallPivotingVertexPtr& v, double radius) {
        utility::LogDebug("[TrySeed] with v.idx={}, radius={}", v->idx_,
                          radius);
        std::vector<int> indices;
        std::vector<double> dists2;
        kdtree_.SearchRadius(v->point_, 2 * radius, indices, dists2);//頂点から半径2*radius内頂点を探す
        if (indices.size() < 3u) {//発見頂点が3つ未満の場合
            return false;
        }

        //発見した頂点を順番にループで調べる．nbidx0の頂点を探す．
        for (size_t nbidx0 = 0; nbidx0 < indices.size(); ++nbidx0) {
            const BallPivotingVertexPtr& nb0 = vertices[indices[nbidx0]];
            if (nb0->type_ != BallPivotingVertex::Type::Orphan) {
                //頂点タイプがOrphanの場合，つまりどのメッシュにも属していいない場合
                continue;
            }
            if (nb0->idx_ == v->idx_) {
                //発見した頂点が引数v頂点と同じ場合
                continue;
            }

            int candidate_vidx2 = -1;
            Eigen::Vector3d center;
            //nbidx0以外の頂点nbidx1を探す
            for (size_t nbidx1 = nbidx0 + 1; nbidx1 < indices.size();
                 ++nbidx1) {
                const BallPivotingVertexPtr& nb1 = vertices[indices[nbidx1]];
                //頂点タイプがOrphanの場合，つまりどのメッシュにも属していいない場合
                if (nb1->type_ != BallPivotingVertex::Type::Orphan) {
                    continue;
                }
                //発見した頂点が引数v頂点と同じ場合
                if (nb1->idx_ == v->idx_) {
                    continue;
                }
                //vとnb0とnb1が三角形になれる場合
                if (TryTriangleSeed(v, nb0, nb1, indices, radius, center)) {//ここで球の中心座標も計算する
                    //candidate_vidx2にnb1のインデックス番号，つまり正の値を代入する．
                    candidate_vidx2 = nb1->idx_;
                    break;
                }
            }

            //candidate_vidx2 が非負の場合，つまりcandidate_vidx2にnb1のインデックス番号が代入された場合
            if (candidate_vidx2 >= 0) {
                const BallPivotingVertexPtr& nb1 = vertices[candidate_vidx2];

                //↓全エッジのタイプがFrontであるかを確認する．なぜならシード三角形なので，全てのエッジはFrontにならなくてはいけない

                BallPivotingEdgePtr e0 = GetLinkingEdge(v, nb1);//e0辺を生成
                //e0が存在して，タイプがFront(つまり境界エッジ)ではない場合
                if (e0 != nullptr &&
                    e0->type_ != BallPivotingEdge::Type::Front) {
                    continue;
                }
                BallPivotingEdgePtr e1 = GetLinkingEdge(nb0, nb1);//e1辺を生成
                //e1が存在して，タイプがFront(つまり境界エッジ)ではない場合
                if (e1 != nullptr &&
                    e1->type_ != BallPivotingEdge::Type::Front) {
                    continue;
                }
                BallPivotingEdgePtr e2 = GetLinkingEdge(v, nb0);//e2辺を生成
                //e2が存在して，タイプがFront(つまり境界エッジ)ではない場合
                if (e2 != nullptr &&
                    e2->type_ != BallPivotingEdge::Type::Front) {
                    continue;
                }

                CreateTriangle(v, nb0, nb1, center);//メッシュを生成，またここで生成した三角形の各辺に各triangle0やtriangle1を登録する．

                e0 = GetLinkingEdge(v, nb1);
                e1 = GetLinkingEdge(nb0, nb1);
                e2 = GetLinkingEdge(v, nb0);
                //e0のタイプがFrontの場合，Frontリストにe0を追加する．
                if (e0->type_ == BallPivotingEdge::Type::Front) {
                    edge_front_.push_front(e0);
                }
                //e1のタイプがFrontの場合，Frontリストにe1を追加する．
                if (e1->type_ == BallPivotingEdge::Type::Front) {
                    edge_front_.push_front(e1);
                }
                //e2のタイプがFrontの場合，Frontリストにe2を追加する．
                if (e2->type_ == BallPivotingEdge::Type::Front) {
                    edge_front_.push_front(e2);
                }

                if (edge_front_.size() > 0) {
                    utility::LogDebug(
                            "[TrySeed] edge_front_.size() > 0 => return "
                            "true");
                    return true;
                }
            }
        }

        utility::LogDebug("[TrySeed] return false");
        return false;
    }

    //引数の半径として，最初の三角形(シード三角形)を見つけて，拡張していく．
    void FindSeedTriangle(double radius) {
        //全点をループで調べる
        for (size_t vidx = 0; vidx < vertices.size(); ++vidx) {
            utility::LogDebug("[FindSeedTriangle] with radius={}, vidx={}",
                              radius, vidx);
            //頂点のタイプがOrphan(メッシュの一部として使われていない)の場合
            if (vertices[vidx]->type_ == BallPivotingVertex::Type::Orphan) {
                //フロントエッジを見つけられた場合
                if (TrySeed(vertices[vidx], radius)) {
                    ExpandTriangulation(radius);
                }
            }
        }
    }

    std::shared_ptr<TriangleMesh> Run(const std::vector<double>& radii) {
        if (!has_normals_) {
            utility::LogError("ReconstructBallPivoting requires normals");
        }

        mesh_->triangles_.clear();//メッシュをクリア

        //与えられた半径を順番に使ってメッシュを生成する
        for (double radius : radii) {
            utility::LogDebug("[Run] ################################");
            utility::LogDebug("[Run] change to radius {:.4f}", radius);
            if (radius <= 0) {
                utility::LogError(
                        "got an invalid, negative radius as parameter");
            }

            // update radius => update border edges
            //最初の半径はこのfor文の工程は行わない．ここは最初の半径の球で作成した面を次の半径の球で生成した面に更新するためにある
            for (auto it = border_edges_.begin(); it != border_edges_.end();) {
                BallPivotingEdgePtr edge = *it;
                BallPivotingTrianglePtr triangle = edge->triangle0_;
                utility::LogDebug(
                        "[Run] try edge {:d}-{:d} of triangle {:d}-{:d}-{:d}",
                        edge->source_->idx_, edge->target_->idx_,
                        triangle->vert0_->idx_, triangle->vert1_->idx_,
                        triangle->vert2_->idx_);

                Eigen::Vector3d center;
                if (ComputeBallCenter(triangle->vert0_->idx_,
                                      triangle->vert1_->idx_,
                                      triangle->vert2_->idx_, radius, center)) {
                    utility::LogDebug("[Run]   yes, we can work on this");
                    std::vector<int> indices;
                    std::vector<double> dists2;
                    kdtree_.SearchRadius(center, radius, indices, dists2);
                    bool empty_ball = true;
                    for (auto idx : indices) {
                        if (idx != triangle->vert0_->idx_ &&
                            idx != triangle->vert1_->idx_ &&
                            idx != triangle->vert2_->idx_) {
                            utility::LogDebug(
                                    "[Run]   but no, the ball is not empty");
                            empty_ball = false;
                            break;
                        }
                    }

                    if (empty_ball) {
                        utility::LogDebug(
                                "[Run]   yeah, add edge to edge_front_: {:d}",
                                edge_front_.size());
                        edge->type_ = BallPivotingEdge::Type::Front;
                        edge_front_.push_back(edge);
                        it = border_edges_.erase(it);
                        continue;
                    }
                }
                ++it;
            }


            // do the reconstruction
            //ここが一番最初の半径が実行する一番最初の処理
            if (edge_front_.empty()) {
                //一番最初の三角形(シード，種)を見つける
                FindSeedTriangle(radius);
            } else {
                //三角形を拡張していく
                ExpandTriangulation(radius);
            }

            utility::LogDebug("[Run] mesh_ has {:d} triangles",
                              mesh_->triangles_.size());
            utility::LogDebug("[Run] ################################");
        }
        return mesh_;
    }

private:
    bool has_normals_;
    KDTreeFlann kdtree_;//最近傍探索などに使用される
    std::list<BallPivotingEdgePtr> edge_front_;
    std::list<BallPivotingEdgePtr> border_edges_;
    std::vector<BallPivotingVertexPtr> vertices;
    std::shared_ptr<TriangleMesh> mesh_;
};

std::shared_ptr<TriangleMesh> TriangleMesh::CreateFromPointCloudBallPivoting(
        const PointCloud& pcd, const std::vector<double>& radii) {
    BallPivoting bp(pcd);
    return bp.Run(radii);
}

}  // namespace geometry
}  // namespace open3d