/*
 * Copyright (c) The Shogun Machine Learning Toolbox
 * Written (w) 2016 Heiko Strathmann
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are those
 * of the authors and should not be interpreted as representing official policies,
 * either expressed or implied, of the Shogun Development Team.
 */

#include <shogun/lib/config.h>
#include <shogun/lib/SGMatrix.h>
#include <shogun/lib/SGVector.h>
#include <shogun/mathematics/eigen3.h>
#include <shogun/mathematics/Math.h>

#include "kernel/Base.h"
#include "Nystrom.h"

// TODO remove this, code should be shared via base class instead
#include "Full.h"

using namespace shogun;
using namespace shogun::kernel_exp_family_impl;
using namespace Eigen;

Nystrom::Nystrom(SGMatrix<float64_t> data,
		kernel::Base* kernel, float64_t lambda, SGVector<index_t> rkhs_basis_inds)  :
		Base(data, kernel, lambda)
{
	m_rkhs_basis_inds = rkhs_basis_inds;

	SG_SINFO("Using m=%d user-defined RKHS basis functions.\n", rkhs_basis_inds.vlen);
}


Nystrom::Nystrom(SGMatrix<float64_t> data,
		kernel::Base* kernel, float64_t lambda,
		index_t num_rkhs_basis)  : Base(data, kernel, lambda)
{
	m_rkhs_basis_inds = sub_sample_rkhs_basis(num_rkhs_basis);
}

index_t Nystrom::get_num_rkhs_basis() const
{
	return m_rkhs_basis_inds.vlen;
}

std::pair<index_t, index_t> Nystrom::idx_to_ai(index_t idx) const
{
	return idx_to_ai(idx, get_num_dimensions());
}

std::pair<index_t, index_t> Nystrom::idx_to_ai(index_t idx, index_t D)
{
	return std::pair<index_t, index_t>(idx / D, idx % D);
}


float64_t Nystrom::compute_xi_norm_2() const
{
	auto N = get_num_lhs();
	auto m = get_num_rkhs_basis();
	auto D = get_num_dimensions();
	float64_t xi_norm_2=0;

#pragma omp parallel for reduction (+:xi_norm_2)
	for (auto idx=0; idx<m; idx++)
		for (auto idx_b=0; idx_b<N; idx_b++)
			for (auto j=0; j<D; j++)
			{
				auto ai = idx_to_ai(m_rkhs_basis_inds[idx]);
				xi_norm_2 += m_kernel->dx_dx_dy_dy_component(ai.first, idx_b, ai.second, j);
			}

	// TODO check math as the number of terms is different here
	xi_norm_2 /= (N*N);

	return xi_norm_2;
}

std::pair<SGMatrix<float64_t>, SGVector<float64_t>> Nystrom::build_system() const
{
	auto D = get_num_dimensions();
	auto N = get_num_lhs();
	auto ND = N*D;
	auto m = get_num_rkhs_basis();

	SG_SINFO("Allocating memory for system.\n");
	SGMatrix<float64_t> A(m+1, m+1);
	Map<MatrixXd> eigen_A(A.matrix, m+1, m+1);
	SGVector<float64_t> b(m+1);
	Map<VectorXd> eigen_b(b.vector, m+1);

	// TODO dont compute full h
	SG_SINFO("Computing h.\n");
	auto h = compute_h();
	auto eigen_h=Map<VectorXd>(h.vector, m);

	SG_SINFO("Computing xi norm.\n");
	auto xi_norm_2 = compute_xi_norm_2();

	SG_SINFO("Creating sub-sampled kernel Hessians.\n");
	SGMatrix<float64_t> col_sub_sampled_hessian(ND, m);
	SGMatrix<float64_t> sub_sampled_hessian(m, m);

	auto eigen_col_sub_sampled_hessian = Map<MatrixXd>(col_sub_sampled_hessian.matrix, ND, m);
	auto eigen_sub_sampled_hessian = Map<MatrixXd>(sub_sampled_hessian.matrix, m, m);

#pragma omp parallel for
	for (auto idx=0; idx<m; idx++)
	{
		auto ai = idx_to_ai(m_rkhs_basis_inds[idx]);

		// TODO vectorise: compute the whole column of all kernel hessians at once
		for (auto row_idx=0; row_idx<ND; row_idx++)
		{
			auto bj = idx_to_ai(row_idx);
			col_sub_sampled_hessian(row_idx, idx)=
					m_kernel->dx_dy_component(ai.first, bj.first, ai.second, bj.second);
		}

		for (auto row_idx=0; row_idx<m; row_idx++)
			sub_sampled_hessian(row_idx,idx)=col_sub_sampled_hessian(m_rkhs_basis_inds[row_idx], idx);
	}

	SG_SINFO("Populating A matrix.\n");
	A(0,0) = eigen_h.squaredNorm() / N + m_lambda * xi_norm_2;

	// can use noalias to speed up as matrices are definitely different
	eigen_A.block(1,1,m,m).noalias()=eigen_col_sub_sampled_hessian.transpose()*eigen_col_sub_sampled_hessian / N + m_lambda*eigen_sub_sampled_hessian;
	eigen_A.col(0).segment(1, m).noalias() = eigen_sub_sampled_hessian*eigen_h / N + m_lambda*eigen_h;

	for (auto idx=0; idx<m; idx++)
		A(0, idx+1) = A(idx+1, 0);

	b[0] = -xi_norm_2;
	eigen_b.segment(1, m) = -eigen_h;

	return std::pair<SGMatrix<float64_t>, SGVector<float64_t>>(A, b);
}

SGVector<float64_t> Nystrom::compute_h() const
{
	auto m = get_num_rkhs_basis();
	auto D = get_num_dimensions();
	auto N = get_num_lhs();

	SGVector<float64_t> h(m);
	Map<VectorXd> eigen_h(h.vector, m);
	eigen_h = VectorXd::Zero(m);

#pragma omp parallel for
	for (auto idx=0; idx<m; idx++)
	{
		auto bj = idx_to_ai(m_rkhs_basis_inds[idx]);

		// TODO compute sum in single go
		// TODO put Nystrom basis as LHS
		for (auto idx_a=0; idx_a<N; idx_a++)
			for (auto i=0; i<D; i++)
				h[idx] += m_kernel->dx_dx_dy_component(idx_a, bj.first, i, bj.second);
	}

	eigen_h /= N;

	return h;
}


float64_t Nystrom::log_pdf(index_t idx_test) const
{
	auto N = get_num_lhs();
	auto m = get_num_rkhs_basis();

	float64_t xi = 0;
	float64_t beta_sum = 0;

	for (auto idx=0; idx<m; idx++)
	{
		auto ai = idx_to_ai(m_rkhs_basis_inds[idx]);

		auto xi_grad_i = m_kernel->dx_dx_component(ai.first, idx_test, ai.second);
		auto grad_x_xa_i = m_kernel->dx_component(ai.first, idx_test, ai.second);

		xi += xi_grad_i;
		// note: sign flip due to swapped kernel arugment compared to Python code
		beta_sum -= grad_x_xa_i * m_alpha_beta[1+idx];
	}

	return m_alpha_beta[0]*xi/N + beta_sum;
}

SGVector<float64_t> Nystrom::grad(index_t idx_test) const
{
	auto N = get_num_lhs();
	auto D = get_num_dimensions();
	auto m = get_num_rkhs_basis();

	SGVector<float64_t> xi_grad(D);
	SGVector<float64_t> beta_sum_grad(D);
	Map<VectorXd> eigen_xi_grad(xi_grad.vector, D);
	Map<VectorXd> eigen_beta_sum_grad(beta_sum_grad.vector, D);
	eigen_xi_grad = VectorXd::Zero(D);
	eigen_beta_sum_grad.array() = VectorXd::Zero(D);

	for (auto idx=0; idx<m; idx++)
	{
		auto ai = idx_to_ai(m_rkhs_basis_inds[idx]);

		auto xi_gradient_mat_component = m_kernel->dx_i_dx_i_dx_j_component(ai.first, idx_test, ai.second);
		Map<VectorXd> eigen_xi_gradient_mat_component(xi_gradient_mat_component.vector, D);
		auto left_arg_hessian_component = m_kernel->dx_i_dx_j_component(ai.first, idx_test, ai.second);
		Map<VectorXd> eigen_left_arg_hessian_component(left_arg_hessian_component.vector, D);

		// note: sign flip due to swapped kernel argument compared to Python code
		eigen_xi_grad -= eigen_xi_gradient_mat_component;
		eigen_beta_sum_grad += eigen_left_arg_hessian_component * m_alpha_beta[1+idx];
	}

	// re-use memory
	eigen_xi_grad *= m_alpha_beta[0] / N;
	eigen_xi_grad += eigen_beta_sum_grad;
	return xi_grad;
}

SGMatrix<float64_t> Nystrom::pinv_self_adjoint(const SGMatrix<float64_t>& A)
{
	// based on the snippet from
	// http://eigen.tuxfamily.org/index.php?title=FAQ#Is_there_a_method_to_compute_the_.28Moore-Penrose.29_pseudo_inverse_.3F
	// modified using eigensolver for psd problems
	auto m=A.num_rows;
	ASSERT(A.num_cols == m);
	auto eigen_A=Map<MatrixXd>(A.matrix, m, m);

	SelfAdjointEigenSolver<MatrixXd> solver(eigen_A);
	auto s = solver.eigenvalues();
	auto V = solver.eigenvectors();

	// tol = eps⋅max(m,n) * max(singularvalues)
	// this is done in numpy/Octave & co
	// c.f. https://en.wikipedia.org/wiki/Moore%E2%80%93Penrose_pseudoinverse#Singular_value_decomposition_.28SVD.29
	float64_t pinv_tol = CMath::MACHINE_EPSILON * m * s.maxCoeff();

	VectorXd inv_s(m);
	for (auto i=0; i<m; i++)
	{
		if (s(i) > pinv_tol)
			inv_s(i)=1.0/s(i);
		else
			inv_s(i)=0;
	}

	SGMatrix<float64_t> A_pinv(m, m);
	Map<MatrixXd> eigen_pinv(A_pinv.matrix, m, m);
	eigen_pinv = (V*inv_s.asDiagonal()*V.transpose());

	return A_pinv;
}

SGMatrix<float64_t> Nystrom::hessian(index_t idx_test) const
{
	auto N = get_num_lhs();
	auto D = get_num_dimensions();
	auto m = get_num_rkhs_basis();

	SGMatrix<float64_t> xi_hessian(D, D);
	SGMatrix<float64_t> beta_sum_hessian(D, D);

	Map<MatrixXd> eigen_xi_hessian(xi_hessian.matrix, D, D);
	Map<MatrixXd> eigen_beta_sum_hessian(beta_sum_hessian.matrix, D, D);

	eigen_xi_hessian = MatrixXd::Zero(D, D);
	eigen_beta_sum_hessian = MatrixXd::Zero(D, D);

	Map<VectorXd> eigen_alpha_beta(m_alpha_beta.vector, m+1);

	// creates a sparse version of the alpha beta vector that has same
	// dimension as in full version
	// TODO can be improved
	VectorXd beta_full_fake=VectorXd::Zero(N*D);
	for (auto idx=0; idx<m; idx++)
	{
		auto ai = idx_to_ai(m_rkhs_basis_inds[idx]);
		beta_full_fake[ai.first*D+ai.second] = m_alpha_beta[1+idx];
	}

	// TODO currently iterates over all data points
	// This should be fine as every data is likely to be in the Nystrom basis
	// However can be done better via only touching the data in the Nystrom basis
	// and for every data check which are the corresponding dimensions that contribute
	// happens implicit here as some parts of the beta vector are zero
	// better to iterate over LHS, ie the Nystrom basis explicitly
	for (auto idx_a=0; idx_a<N; idx_a++)
	{
		auto xi_hess_sum = m_kernel->dx_i_dx_j_dx_k_dx_k_row_sum(idx_a, idx_test);

		Map<MatrixXd> eigen_xi_hess_sum(xi_hess_sum.matrix, D, D);
		eigen_xi_hessian += eigen_xi_hess_sum;

		SGVector<float64_t> beta_a(beta_full_fake.segment(idx_a*D, D).data(), D, false);

		// Note sign flip because arguments are opposite order of Python code
		auto beta_hess_sum = m_kernel->dx_i_dx_j_dx_k_dot_vec(idx_a, idx_test, beta_a);
		Map<MatrixXd> eigen_beta_hess_sum(beta_hess_sum.matrix, D, D);
		eigen_beta_sum_hessian -= eigen_beta_hess_sum;
	}

	eigen_xi_hessian.array() *= m_alpha_beta[0] / N;

	// re-use memory rather than re-allocating a new result matrix
	eigen_xi_hessian += eigen_beta_sum_hessian;

	return xi_hessian;
}

SGVector<float64_t> Nystrom::hessian_diag(index_t idx_test) const
{
	// Note: code modifed from full hessian case
	auto N = get_num_lhs();
	auto D = get_num_dimensions();
	auto m = get_num_rkhs_basis();

	SGVector<float64_t> xi_hessian_diag(D);
	SGVector<float64_t> beta_sum_hessian_diag(D);

	Map<VectorXd> eigen_xi_hessian_diag(xi_hessian_diag.vector, D);
	Map<VectorXd> eigen_beta_sum_hessian_diag(beta_sum_hessian_diag.vector, D);

	eigen_xi_hessian_diag = VectorXd::Zero(D);
	eigen_beta_sum_hessian_diag = VectorXd::Zero(D);

	Map<VectorXd> eigen_alpha_beta(m_alpha_beta.vector, m+1);

	// TODO can be improved (see full hessian case)
	VectorXd beta_full_fake=VectorXd::Zero(N*D);
	for (auto idx=0; idx<m; idx++)
	{
		auto ai = idx_to_ai(m_rkhs_basis_inds[idx]);
		beta_full_fake[ai.first*D+ai.second] = m_alpha_beta[1+idx];
	}

	// TODO currently iterates over all data points (see full hessian case)
	SGVector<float64_t> xi_hess_sum_diag = SGVector<float64_t>(D);
	for (auto idx_a=0; idx_a<N; idx_a++)
	{
		SGVector<float64_t> beta_a(beta_full_fake.segment(idx_a*D, D).data(), D, false);
		for (auto i=0; i<D; i++)
		{
			eigen_xi_hessian_diag[i] += m_kernel->dx_i_dx_j_dx_k_dx_k_row_sum_component(idx_a, idx_test, i, i);
			eigen_beta_sum_hessian_diag[i] -= m_kernel->dx_i_dx_j_dx_k_dot_vec_component(idx_a, idx_test, beta_a, i, i);
		}
	}

	eigen_xi_hessian_diag.array() *= m_alpha_beta[0] / N;
	eigen_xi_hessian_diag += eigen_beta_sum_hessian_diag;

	return xi_hessian_diag;
}

SGVector<float64_t> Nystrom::leverage() const
{
	SG_SERROR("Not implemented yet.\n");
	return SGVector<float64_t>();
}


// new nystrom implementation

SGVector<index_t> Nystrom::sub_sample_rkhs_basis(index_t num_rkhs_basis) const
{
	SG_SINFO("Using m=%d uniformly sampled RKHS basis functions.\n", num_rkhs_basis);
	auto N = get_num_lhs();
	auto D = get_num_dimensions();

	SGVector<index_t> permutation(N*D);
	permutation.range_fill();
	CMath::permute(permutation);
	auto rkhs_basis_inds = SGVector<index_t>(num_rkhs_basis);
	for (auto i=0; i<num_rkhs_basis; i++)
		rkhs_basis_inds[i]=permutation[i];

	// in order to have more sequential data reads
	CMath::qsort(rkhs_basis_inds.vector, num_rkhs_basis);

	return rkhs_basis_inds;
}

