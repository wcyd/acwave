#include "acoustic_wave.hpp"
#include "parameters.hpp"
#include "utilities.hpp"

#include <float.h>

using namespace std;
using namespace mfem;

//#define BASIS_DG



#ifdef MFEM_USE_MPI
void AcousticWave::run_GMsFEM() const
{
//  int size;
//  MPI_Comm_size(MPI_COMM_WORLD, &size);
//  if (size == 1)
//    run_GMsFEM_serial();
//  else
    run_GMsFEM_parallel();
}
#endif // MFEM_USE_MPI



void fill_up_n_fine_cells_per_coarse(int n_fine, int n_coarse,
                                     std::vector<int> &result)
{
  const int k = n_fine / n_coarse;
  for (size_t i = 0; i < result.size(); ++i)
    result[i] = k;
  const int p = n_fine % n_coarse;
  for (int i = 0; i < p; ++i)
    ++result[i];
}



static void time_step(const SparseMatrix &M, const SparseMatrix &S,
                      const Vector &b, double timeval, double dt,
                      const SparseMatrix &SysMat, Solver &Prec,
                      Vector &U_0, Vector &U_1, Vector &U_2)
{
  Vector y = U_1; y *= 2.0; y -= U_2;        // y = 2*u_1 - u_2

  Vector z0; z0.SetSize(U_0.Size());         // z0 = M * (2*u_1 - u_2)
  M.Mult(y, z0);

  Vector z1; z1.SetSize(U_0.Size()); S.Mult(U_1, z1); // z1 = S * u_1
  Vector z2 = b; z2 *= timeval; // z2 = timeval*source

  // y = dt^2 * (S*u_1 - timeval*source), where it can be
  // y = dt^2 * (S*u_1 - ricker*pointforce) OR
  // y = dt^2 * (S*u_1 - gaussfirstderivative*momenttensor)
  y = z1; y -= z2; y *= dt*dt;

  // RHS = M*(2*u_1-u_2) - dt^2*(S*u_1-timeval*source)
  Vector RHS = z0; RHS -= y;

//    for (int i = 0; i < N; ++i) y[i] = diagD[i] * u_2[i]; // y = D * u_2

  // RHS = M*(2*u_1-u_2) - dt^2*(S*u_1-timeval*source) + D*u_2
//    RHS += y;

  // (M+D)*x_0 = M*(2*x_1-x_2) - dt^2*(S*x_1-r*b) + D*x_2
  PCG(SysMat, Prec, RHS, U_0, 0, 200, 1e-12, 0.0);

  U_2 = U_1;
  U_1 = U_0;
}



#ifdef MFEM_USE_MPI
void AcousticWave::run_GMsFEM_serial() const
{
  MFEM_VERIFY(param.mesh, "The mesh is not initialized");

  StopWatch chrono;
  chrono.Start();

  const int dim = param.dimension;
  const int n_elements = param.mesh->GetNE();

  cout << "FE space generation..." << flush;
  FiniteElementCollection *fec = new DG_FECollection(param.method.order, dim);
  FiniteElementSpace fespace(param.mesh, fec);
  cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
  chrono.Clear();

  cout << "Number of unknowns: " << fespace.GetVSize() << endl;

  double *one_over_rho = new double[n_elements]; // one over density
  double *one_over_K   = new double[n_elements]; // one over bulk modulus

  double Rho[] = { DBL_MAX, DBL_MIN };
  double Vp[]  = { DBL_MAX, DBL_MIN };
  double Kap[] = { DBL_MAX, DBL_MIN };

  for (int i = 0; i < n_elements; ++i)
  {
    const double rho = param.media.rho_array[i];
    const double vp  = param.media.vp_array[i];
    const double K   = rho*vp*vp;

    MFEM_VERIFY(rho > 0. && vp > 0., "Incorrect media properties arrays");

    Rho[0] = std::min(Rho[0], rho);
    Rho[1] = std::max(Rho[1], rho);
    Vp[0]  = std::min(Vp[0], vp);
    Vp[1]  = std::max(Vp[1], vp);
    Kap[0] = std::min(Kap[0], K);
    Kap[1] = std::max(Kap[1], K);

    one_over_rho[i] = 1. / rho;
    one_over_K[i]   = 1. / K;
  }

  std::cout << "Rho: min " << Rho[0] << " max " << Rho[1] << "\n";
  std::cout << "Vp:  min " << Vp[0]  << " max " << Vp[1] << "\n";
  std::cout << "Kap: min " << Kap[0] << " max " << Kap[1] << "\n";

  const bool own_array = true;
  CWConstCoefficient one_over_rho_coef(one_over_rho, own_array);
  CWConstCoefficient one_over_K_coef(one_over_K, own_array);

  cout << "Fine scale stif matrix..." << flush;
  BilinearForm stif_fine(&fespace);
  stif_fine.AddDomainIntegrator(new DiffusionIntegrator(one_over_rho_coef));
  stif_fine.AddInteriorFaceIntegrator(
        new DGDiffusionIntegrator(one_over_rho_coef,
                                  param.method.dg_sigma,
                                  param.method.dg_kappa));
  stif_fine.AddBdrFaceIntegrator(
        new DGDiffusionIntegrator(one_over_rho_coef,
                                  param.method.dg_sigma,
                                  param.method.dg_kappa));
  stif_fine.Assemble();
  stif_fine.Finalize();
  const SparseMatrix& S_fine = stif_fine.SpMat();
  cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
  chrono.Clear();

  cout << "Fine scale mass matrix..." << flush;
  BilinearForm mass_fine(&fespace);
  mass_fine.AddDomainIntegrator(new MassIntegrator(one_over_K_coef));
  mass_fine.Assemble();
  mass_fine.Finalize();
  const SparseMatrix& M_fine = mass_fine.SpMat();
  cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
  chrono.Clear();

//  cout << "Damp matrix..." << flush;
//  VectorMassIntegrator *damp_int = new VectorMassIntegrator(rho_damp_coef);
//  damp_int->SetIntRule(GLL_rule);
//  BilinearForm dampM(&fespace);
//  dampM.AddDomainIntegrator(damp_int);
//  dampM.Assemble();
//  dampM.Finalize();
//  SparseMatrix& D = dampM.SpMat();
//  double omega = 2.0*M_PI*param.source.frequency; // angular frequency
//  D *= 0.5*param.dt*omega;
//  cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
//  chrono.Clear();

  SparseMatrix SysFine(M_fine);
  SysFine = 0.0;
  //SysFine += D;
  SysFine += M_fine;
  GSSmoother PrecFine(SysFine);

  cout << "Fine scale RHS vector... " << flush;
  LinearForm b_fine(&fespace);
  ConstantCoefficient zero(0.0); // homogeneous Dirichlet bc
  if (param.source.plane_wave)
  {
    PlaneWaveSource plane_wave_source(param, one_over_K_coef);
    b_fine.AddDomainIntegrator(new DomainLFIntegrator(plane_wave_source));
    b_fine.AddBdrFaceIntegrator(
          new DGDirichletLFIntegrator(zero, one_over_rho_coef,
                                      param.method.dg_sigma,
                                      param.method.dg_kappa));
    b_fine.Assemble();
  }
  else
  {
    ScalarPointForce scalar_point_force(param, one_over_K_coef);
    b_fine.AddDomainIntegrator(new DomainLFIntegrator(scalar_point_force));
    b_fine.AddBdrFaceIntegrator(
          new DGDirichletLFIntegrator(zero, one_over_rho_coef,
                                      param.method.dg_sigma,
                                      param.method.dg_kappa));
    b_fine.Assemble();
  }
  cout << "||b_h||_L2 = " << b_fine.Norml2() << endl;
  cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
  chrono.Clear();


  std::vector<int> n_fine_cell_per_coarse_x(param.method.gms_Nx);
  fill_up_n_fine_cells_per_coarse(param.grid.nx, param.method.gms_Nx,
                                  n_fine_cell_per_coarse_x);

  std::vector<int> n_fine_cell_per_coarse_y(param.method.gms_Ny);
  fill_up_n_fine_cells_per_coarse(param.grid.ny, param.method.gms_Ny,
                                  n_fine_cell_per_coarse_y);

  const double hx = param.grid.get_hx();
  const double hy = param.grid.get_hy();

  const int gen_edges = 1;

  std::vector<std::vector<int> > local2global;
  std::vector<DenseMatrix> R;

  if (param.dimension == 2)
  {
    const int n_coarse_cells = param.method.gms_Nx * param.method.gms_Ny;
    local2global.resize(n_coarse_cells);
    R.resize(n_coarse_cells);

    int offset_x, offset_y = 0;

    for (int iy = 0; iy < param.method.gms_Ny; ++iy)
    {
      const int n_fine_y = n_fine_cell_per_coarse_y[iy];
      const double SY = n_fine_y * hy;

      offset_x = 0;
      for (int ix = 0; ix < param.method.gms_Nx; ++ix)
      {
        const int n_fine_x = n_fine_cell_per_coarse_x[ix];
        const double SX = n_fine_x * hx;
        Mesh *ccell_fine_mesh =
            new Mesh(n_fine_x, n_fine_y, Element::QUADRILATERAL, gen_edges, SX, SY);

        double *local_one_over_rho = new double[n_fine_x * n_fine_y];
        double *local_one_over_K   = new double[n_fine_x * n_fine_y];
        for (int fiy = 0; fiy < n_fine_y; ++fiy)
        {
          for (int fix = 0; fix < n_fine_x; ++fix)
          {
            const int loc_cell = fiy*n_fine_x + fix;
            const int glob_cell = (offset_y + fiy) * param.grid.nx +
                                  (offset_x + fix);

            local_one_over_rho[loc_cell] = one_over_rho[glob_cell];
            local_one_over_K[loc_cell]   = one_over_K[glob_cell];
          }
        }
        const bool own_array = true;
        CWConstCoefficient local_one_over_rho_coef(local_one_over_rho, own_array);
        CWConstCoefficient local_one_over_K_coef(local_one_over_K, own_array);

        const int coarse_cell = iy*param.method.gms_Nx + ix;

#ifdef BASIS_DG
        compute_basis_DG(ccell_fine_mesh, param.method.gms_nb, param.method.gms_ni,
                         local_one_over_rho_coef, local_one_over_K_coef,
                         R[coarse_cell]);
#else
        compute_basis_CG(ccell_fine_mesh, param.method.gms_nb, param.method.gms_ni,
                         local_one_over_rho_coef, local_one_over_K_coef,
                         R[coarse_cell]);
#endif

        // initialize with all -1 to check that all values are defined later
        local2global[coarse_cell].resize(R[coarse_cell].Height(), -1);
        DG_FECollection DG_fec(param.method.order, param.dimension);
        FiniteElementSpace DG_fespace(ccell_fine_mesh, &DG_fec);
        Array<int> loc_dofs, glob_dofs;
        for (int fiy = 0; fiy < n_fine_y; ++fiy)
        {
          for (int fix = 0; fix < n_fine_x; ++fix)
          {
            const int loc_cell = fiy*n_fine_x + fix;
            const int glob_cell = (offset_y + fiy) * param.grid.nx +
                                  (offset_x + fix);

            DG_fespace.GetElementVDofs(loc_cell, loc_dofs);
            fespace.GetElementVDofs(glob_cell, glob_dofs);
            MFEM_VERIFY(loc_dofs.Size() == glob_dofs.Size(), "Dimensions mismatch");

            for (int di = 0; di < loc_dofs.Size(); ++di)
              local2global[coarse_cell][loc_dofs[di]] = glob_dofs[di];
          }
        }

        // check that all values were defined
        for (size_t ii = 0; ii < local2global[coarse_cell].size(); ++ii) {
          MFEM_VERIFY(local2global[coarse_cell][ii] >= 0, "Some values of local2global "
                      "vector were not defined");
        }

        delete ccell_fine_mesh;

        offset_x += n_fine_x;
      }
      offset_y += n_fine_y;
    }
  }
  else // 3D
  {
    std::vector<int> n_fine_cell_per_coarse_z(param.method.gms_Nz);
    fill_up_n_fine_cells_per_coarse(param.grid.nz, param.method.gms_Nz,
                                    n_fine_cell_per_coarse_z);

    const double hz = param.grid.get_hz();

    const int n_coarse_cells = param.method.gms_Nx * param.method.gms_Ny * param.method.gms_Nz;
    local2global.resize(n_coarse_cells);
    R.resize(n_coarse_cells);

    int offset_x = 0, offset_y = 0, offset_z = 0;

    for (int iz = 0; iz < param.method.gms_Nz; ++iz)
    {
      const int n_fine_z = n_fine_cell_per_coarse_z[iz];
      const double SZ = n_fine_z * hz;
      for (int iy = 0; iy < param.method.gms_Ny; ++iy)
      {
        const int n_fine_y = n_fine_cell_per_coarse_y[iy];
        const double SY = n_fine_y * hy;
        for (int ix = 0; ix < param.method.gms_Nx; ++ix)
        {
          const int n_fine_x = n_fine_cell_per_coarse_x[ix];
          const double SX = n_fine_x * hx;
          Mesh *ccell_fine_mesh =
              new Mesh(n_fine_cell_per_coarse_x[ix],
                       n_fine_cell_per_coarse_y[iy],
                       n_fine_cell_per_coarse_z[iz],
                       Element::HEXAHEDRON, gen_edges, SX, SY, SZ);

          double *local_one_over_rho = new double[n_fine_x * n_fine_y * n_fine_z];
          double *local_one_over_K   = new double[n_fine_x * n_fine_y * n_fine_z];
          for (int fiz = 0; fiz < n_fine_z; ++fiz)
          {
            for (int fiy = 0; fiy < n_fine_y; ++fiy)
            {
              for (int fix = 0; fix < n_fine_x; ++fix)
              {
                const int loc_cell = fiz*n_fine_x*n_fine_y + fiy*n_fine_x + fix;
                const int glob_cell = (offset_z + fiz) * param.grid.nx * param.grid.ny +
                                      (offset_y + fiy) * param.grid.nx +
                                      (offset_x + fix);

                local_one_over_rho[loc_cell] = one_over_rho[glob_cell];
                local_one_over_K[loc_cell]   = one_over_K[glob_cell];
              }
            }
          }
          const bool own_array = true;
          CWConstCoefficient local_one_over_rho_coef(local_one_over_rho, own_array);
          CWConstCoefficient local_one_over_K_coef(local_one_over_K, own_array);

#ifdef BASIS_DG
          compute_basis_DG(ccell_fine_mesh, param.method.gms_nb, param.method.gms_ni,
                           local_one_over_rho_coef, local_one_over_K_coef,
                           R[iz*param.method.gms_Nx*param.method.gms_Ny +
                             iy*param.method.gms_Nx + ix]);
#else
          compute_basis_CG(ccell_fine_mesh, param.method.gms_nb, param.method.gms_ni,
                           local_one_over_rho_coef, local_one_over_K_coef,
                           R[iz*param.method.gms_Nx*param.method.gms_Ny +
                             iy*param.method.gms_Nx + ix]);
#endif
          delete ccell_fine_mesh;

          offset_x += n_fine_x;
        }
        offset_y += n_fine_y;
      }
      offset_z += n_fine_z;
    }
  }

  // global sparse R matrix
  int n_rows = 0;
  int n_cols = 0;
  int n_non_zero = 0;
  for (size_t i = 0; i < R.size(); ++i)
  {
    const int h = R[i].Height();
    const int w = R[i].Width();
    n_rows += w; // transpose
    n_cols += h; // transpose
    n_non_zero += h * w;
  }
  MFEM_VERIFY(n_cols == S_fine.Height(), "Dimensions mismatch");
  int *Ri = new int[n_rows + 1];
  int *Rj = new int[n_non_zero];
  double *Rdata = new double[n_non_zero];

  Ri[0] = 0;
  int k = 0;
  int p = 0;
//  int offset = 0;
  for (size_t r = 0; r < R.size(); ++r)
  {
    const int h = R[r].Height();
    const int w = R[r].Width();
    for (int i = 0; i < w; ++i)
    {
      Ri[k+1] = Ri[k] + h;
      ++k;

      for (int j = 0; j < h; ++j)
      {
        Rj[p] = local2global[r][j];
        Rdata[p] = R[r](j, i);
        ++p;
      }
    }
//    offset += h;
  }

  SparseMatrix R_global(Ri, Rj, Rdata, n_rows, n_cols);

  SparseMatrix *R_global_T = Transpose(R_global);

  SparseMatrix *M_coarse = RAP(M_fine, R_global);
  SparseMatrix *S_coarse = RAP(S_fine, R_global);

  Vector b_coarse(M_coarse->Height());
  R_global.Mult(b_fine, b_coarse);

  SparseMatrix SysCoarse(*M_coarse);
  SysCoarse = 0.0;
  //SysCoarse += D;
  SysCoarse += *M_coarse;
  GSSmoother PrecCoarse(SysCoarse);

  if (param.output.print_matrices)
  {
    {
      chrono.Clear();
      cout << "Output R local matrices..." << flush;
      for (size_t r = 0; r < R.size(); ++r) {
        const string fname = string(param.output.directory) + "/r" + d2s(r) +
                             "_local_ser" + param.output.extra_string + ".dat";
        ofstream mout(fname.c_str());
        MFEM_VERIFY(mout, "Cannot open file " + fname);
        R[r].PrintMatlab(mout);
      }
      cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
    }
    {
      chrono.Clear();
      cout << "Output R_global matrix..." << flush;
      const string fname = string(param.output.directory) + "/r_global_ser"
                           + param.output.extra_string + ".dat";
      ofstream mout(fname.c_str());
      MFEM_VERIFY(mout, "Cannot open file " + fname);
      R_global.PrintMatlab(mout);
      cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
    }
    {
      chrono.Clear();
      cout << "Output R_global_T matrix..." << flush;
      const string fname = string(param.output.directory) + "/r_global_t_ser" +
                           param.output.extra_string + ".dat";
      ofstream mout(fname.c_str());
      MFEM_VERIFY(mout, "Cannot open file " + fname);
      R_global_T->PrintMatlab(mout);
      cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
    }
    {
      chrono.Clear();
      cout << "Output M_fine matrix..." << flush;
      const string fname = string(param.output.directory) + "/m_fine_ser" +
                           param.output.extra_string + ".dat";
      ofstream mout(fname.c_str());
      MFEM_VERIFY(mout, "Cannot open file " + fname);
      M_fine.PrintMatlab(mout);
      cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
    }
    {
      chrono.Clear();
      cout << "Output S_fine matrix..." << flush;
      const string fname = string(param.output.directory) + "/s_fine_ser" +
                           param.output.extra_string + ".dat";
      ofstream mout(fname.c_str());
      MFEM_VERIFY(mout, "Cannot open file " + fname);
      S_fine.PrintMatlab(mout);
      cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
    }
    {
      chrono.Clear();
      cout << "Output M_coarse matrix..." << flush;
      const string fname = string(param.output.directory) + "/m_coarse_ser" +
                           param.output.extra_string + ".dat";
      ofstream mout(fname.c_str());
      MFEM_VERIFY(mout, "Cannot open file " + fname);
      M_coarse->PrintMatlab(mout);
      cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
    }
    {
      chrono.Clear();
      cout << "Output S_coarse matrix..." << flush;
      const string fname = string(param.output.directory) + "/s_coarse_ser" +
                           param.output.extra_string + ".dat";
      ofstream mout(fname.c_str());
      MFEM_VERIFY(mout, "Cannot open file " + fname);
      S_coarse->PrintMatlab(mout);
      cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
    }
  }

  const string method_name = "GMsFEM_";

  cout << "Open seismograms files..." << flush;
  ofstream *seisU; // for pressure
  open_seismo_outs(seisU, param, method_name);
  cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
  chrono.Clear();

  Vector U_0(M_coarse->Height()); // coarse scale pressure
  U_0 = 0.0;
  Vector U_1 = U_0;
  Vector U_2 = U_0;
  GridFunction u_0(&fespace); // fine scale projected from coarse scale

  GridFunction u_fine_0(&fespace); // fine scale pressure
  u_fine_0 = 0.0;
  Vector u_fine_1 = u_fine_0;
  Vector u_fine_2 = u_fine_0;

  const int n_time_steps = param.T / param.dt + 0.5; // nearest integer
  const int tenth = 0.1 * n_time_steps;

  cout << "N time steps = " << n_time_steps
       << "\nTime loop..." << endl;

  // the values of the time-dependent part of the source
  vector<double> time_values(n_time_steps);
  for (int time_step = 1; time_step <= n_time_steps; ++time_step)
  {
    const double cur_time = time_step * param.dt;
    time_values[time_step-1] = RickerWavelet(param.source,
                                             cur_time - param.dt);
  }

  const string name = method_name + param.output.extra_string;
  const string pref_path = string(param.output.directory) + "/" + SNAPSHOTS_DIR;
  VisItDataCollection visit_dc(name.c_str(), param.mesh);
  visit_dc.SetPrefixPath(pref_path.c_str());
  visit_dc.RegisterField("fine_pressure", &u_fine_0);
  visit_dc.RegisterField("coarse_pressure", &u_0);
  {
    visit_dc.SetCycle(0);
    visit_dc.SetTime(0.0);
    Vector u_tmp(u_fine_0.Size());
    R_global_T->Mult(U_0, u_tmp);
    u_0.MakeRef(&fespace, u_tmp, 0);
    visit_dc.Save();
  }

  StopWatch time_loop_timer;
  time_loop_timer.Start();
  double time_of_snapshots = 0.;
  double time_of_seismograms = 0.;
  for (int t_step = 1; t_step <= n_time_steps; ++t_step)
  {
    {
      time_step(*M_coarse, *S_coarse, b_coarse, time_values[t_step-1],
                param.dt, SysCoarse, PrecCoarse, U_0, U_1, U_2);
    }
    {
//      time_step(M_fine, S_fine, b_fine, time_values[t_step-1],
//                param.dt, SysFine, PrecFine, u_fine_0, u_fine_1, u_fine_2);
    }

    // Compute and print the L^2 norm of the error
    if (t_step % tenth == 0) {
      cout << "step " << t_step << " / " << n_time_steps
           << " ||U||_{L^2} = " << U_0.Norml2()
           << " ||u||_{L^2} = " << u_fine_0.Norml2() << endl;
    }

    if (t_step % param.step_snap == 0) {
      StopWatch timer;
      timer.Start();
      visit_dc.SetCycle(t_step);
      visit_dc.SetTime(t_step*param.dt);
      Vector u_tmp(u_fine_0.Size());
      R_global_T->Mult(U_0, u_tmp);
      u_0.MakeRef(&fespace, u_tmp, 0);
      visit_dc.Save();
      timer.Stop();
      time_of_snapshots += timer.UserTime();
    }

//    if (t_step % param.step_seis == 0) {
//      StopWatch timer;
//      timer.Start();
//      R_global_T.Mult(U_0, u_0);
//      output_seismograms(param, *param.mesh, u_0, seisU);
//      timer.Stop();
//      time_of_seismograms += timer.UserTime();
//    }
  }

  time_loop_timer.Stop();

  delete[] seisU;

  cout << "Time loop is over\n\tpure time = " << time_loop_timer.UserTime()
       << "\n\ttime of snapshots = " << time_of_snapshots
       << "\n\ttime of seismograms = " << time_of_seismograms << endl;

  delete S_coarse;
  delete M_coarse;
  delete R_global_T;

  delete fec;
}
#endif // MFEM_USE_MPI



#ifdef MFEM_USE_MPI
static void par_time_step(HypreParMatrix &M, HypreParMatrix &S,
                          const Vector &b, double timeval, double dt,
                          Vector &U_0, Vector &U_1, Vector &U_2)
{
  HypreSmoother M_prec;
  CGSolver M_solver(M.GetComm());

  M_prec.SetType(HypreSmoother::Jacobi);
  M_solver.SetPreconditioner(M_prec);
  M_solver.SetOperator(M);

  M_solver.iterative_mode = false;
  M_solver.SetRelTol(1e-12);
  M_solver.SetAbsTol(0.0);
  M_solver.SetMaxIter(200);
  M_solver.SetPrintLevel(0);

  Vector y = U_1; y *= 2.0; y -= U_2;        // y = 2*u_1 - u_2

  Vector z0 = U_0;         // z0 = M * (2*u_1 - u_2)
  M.Mult(y, z0);

  Vector z1 = U_0;         // z1 = S * u_1
  S.Mult(U_1, z1);

  Vector z2 = b; z2 *= timeval; // z2 = timeval*source

  // y = dt^2 * (S*u_1 - timeval*source), where it can be
  // y = dt^2 * (S*u_1 - ricker*pointforce) OR
  // y = dt^2 * (S*u_1 - gaussfirstderivative*momenttensor)
  y = z1; y -= z2; y *= dt*dt;

  // RHS = M*(2*u_1-u_2) - dt^2*(S*u_1-timeval*source)
  Vector RHS = z0; RHS -= y;

//    for (int i = 0; i < N; ++i) y[i] = diagD[i] * u_2[i]; // y = D * u_2

  // RHS = M*(2*u_1-u_2) - dt^2*(S*u_1-timeval*source) + D*u_2
//    RHS += y;

  // (M+D)*x_0 = M*(2*x_1-x_2) - dt^2*(S*x_1-r*b) + D*x_2
  M_solver.Mult(RHS, U_0);

  U_2 = U_1;
  U_1 = U_0;
}

static void print_par_matrix_matlab(HypreParMatrix &A, const string &filename)
{
  int myid;
  MPI_Comm_rank(MPI_COMM_WORLD, &myid);

  // This call works because HypreParMatrix implicitly converts to hypre_ParCSRMatrix*
  hypre_CSRMatrix* A_serial = hypre_ParCSRMatrixToCSRMatrixAll(A);

  // This "views" the hypre_CSRMatrix as an mfem::SparseMatrix
  mfem::SparseMatrix A_sparse(
        hypre_CSRMatrixI(A_serial), hypre_CSRMatrixJ(A_serial), hypre_CSRMatrixData(A_serial),
        hypre_CSRMatrixNumRows(A_serial), hypre_CSRMatrixNumCols(A_serial),
        false, false, true);

  // Write to file from root process
  if (myid == 0)
  {
    ofstream out(filename.c_str());
    MFEM_VERIFY(out, "Cannot open file " << filename);
    A_sparse.PrintMatlab(out);
  }

  // Cleanup, since the hypre call creates a new matrix on each process
  hypre_CSRMatrixDestroy(A_serial);
}

void AcousticWave::run_GMsFEM_parallel() const
{
  MFEM_VERIFY(param.mesh, "The serial mesh is not initialized");
  MFEM_VERIFY(param.par_mesh, "The parallel mesh is not initialized");

  int myid, nproc;
  MPI_Comm_rank(MPI_COMM_WORLD, &myid);
  MPI_Comm_size(MPI_COMM_WORLD, &nproc);

  string fileout = string(param.output.directory) + "/outputlog." + d2s(myid);
  ofstream out(fileout.c_str());
  MFEM_VERIFY(out, "Cannot open file " << fileout);

  StopWatch chrono;
  chrono.Start();

  const int dim = param.dimension;
  const int n_elements = param.mesh->GetNE();

  out << "FE space generation..." << flush;
  chrono.Clear();
  DG_FECollection fec(param.method.order, dim);
  ParFiniteElementSpace fespace(param.par_mesh, &fec);
  FiniteElementSpace fespace_serial(param.mesh, &fec);
  out << "done. Time = " << chrono.RealTime() << " sec" << endl;

  const int n_dofs = fespace.GlobalTrueVSize();
  out << "Number of unknowns: " << n_dofs << endl;

  double *one_over_rho = new double[n_elements]; // one over density
  double *one_over_K   = new double[n_elements]; // one over bulk modulus

  double Rho[] = { DBL_MAX, DBL_MIN };
  double Vp[]  = { DBL_MAX, DBL_MIN };
  double Kap[] = { DBL_MAX, DBL_MIN };

  for (int i = 0; i < n_elements; ++i)
  {
    const double rho = param.media.rho_array[i];
    const double vp  = param.media.vp_array[i];
    const double K   = rho*vp*vp;

    MFEM_VERIFY(rho > 0. && vp > 0., "Incorrect media properties arrays");

    Rho[0] = std::min(Rho[0], rho);
    Rho[1] = std::max(Rho[1], rho);
    Vp[0]  = std::min(Vp[0], vp);
    Vp[1]  = std::max(Vp[1], vp);
    Kap[0] = std::min(Kap[0], K);
    Kap[1] = std::max(Kap[1], K);

    one_over_rho[i] = 1. / rho;
    one_over_K[i]   = 1. / K;
  }

  out << "Rho: min " << Rho[0] << " max " << Rho[1] << endl;
  out << "Vp:  min " << Vp[0]  << " max " << Vp[1] << endl;
  out << "Kap: min " << Kap[0] << " max " << Kap[1] << endl;

  const bool own_array = true;
  CWConstCoefficient one_over_rho_coef(one_over_rho, own_array);
  CWConstCoefficient one_over_K_coef(one_over_K, own_array);

  out << "Fine scale stif matrix..." << flush;
  chrono.Clear();
  ParBilinearForm stif_fine(&fespace);
  stif_fine.AddDomainIntegrator(new DiffusionIntegrator(one_over_rho_coef));
  stif_fine.AddInteriorFaceIntegrator(
        new DGDiffusionIntegrator(one_over_rho_coef,
                                  param.method.dg_sigma,
                                  param.method.dg_kappa));
  stif_fine.AddBdrFaceIntegrator(
        new DGDiffusionIntegrator(one_over_rho_coef,
                                  param.method.dg_sigma,
                                  param.method.dg_kappa));
  stif_fine.Assemble();
  stif_fine.Finalize();
  HypreParMatrix *S_fine = stif_fine.ParallelAssemble();
  out << "done. Time = " << chrono.RealTime() << " sec" << endl;


  out << "Fine scale mass matrix..." << flush;
  chrono.Clear();
  ParBilinearForm mass_fine(&fespace);
  mass_fine.AddDomainIntegrator(new MassIntegrator(one_over_K_coef));
  mass_fine.Assemble();
  mass_fine.Finalize();
  HypreParMatrix *M_fine = mass_fine.ParallelAssemble();
  out << "done. Time = " << chrono.RealTime() << " sec" << endl;

//  cout << "Damp matrix..." << flush;
//  VectorMassIntegrator *damp_int = new VectorMassIntegrator(rho_damp_coef);
//  damp_int->SetIntRule(GLL_rule);
//  BilinearForm dampM(&fespace);
//  dampM.AddDomainIntegrator(damp_int);
//  dampM.Assemble();
//  dampM.Finalize();
//  SparseMatrix& D = dampM.SpMat();
//  double omega = 2.0*M_PI*param.source.frequency; // angular frequency
//  D *= 0.5*param.dt*omega;
//  cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
//  chrono.Clear();

  //SparseMatrix SysFine(M_fine);
  //SysFine = 0.0;
  //SysFine += D;
  //SysFine += M_fine;
  //GSSmoother PrecFine(SysFine);

  out << "Fine scale RHS vector... " << flush;
  chrono.Clear();
  ParLinearForm b_fine(&fespace);
  ConstantCoefficient zero(0.0); // homogeneous Dirichlet bc
  if (param.source.plane_wave)
  {
    PlaneWaveSource plane_wave_source(param, one_over_K_coef);
    b_fine.AddDomainIntegrator(new DomainLFIntegrator(plane_wave_source));
    b_fine.AddBdrFaceIntegrator(
          new DGDirichletLFIntegrator(zero, one_over_rho_coef,
                                      param.method.dg_sigma,
                                      param.method.dg_kappa));
    b_fine.Assemble();
  }
  else
  {
    ScalarPointForce scalar_point_force(param, one_over_K_coef);
    b_fine.AddDomainIntegrator(new DomainLFIntegrator(scalar_point_force));
    b_fine.AddBdrFaceIntegrator(
          new DGDirichletLFIntegrator(zero, one_over_rho_coef,
                                      param.method.dg_sigma,
                                      param.method.dg_kappa));
    b_fine.Assemble();
  }
  HypreParVector *B_fine = b_fine.ParallelAssemble();
  const double b_fine_norm = GlobalLpNorm(2, B_fine->Norml2(), MPI_COMM_WORLD);
  out << "||b_h||_L2 = " << b_fine_norm << endl;
  out << "done. Time = " << chrono.RealTime() << " sec" << endl;


  out << "serial fespace:\n";
  for (int el = 0; el < fespace_serial.GetNE(); ++el)
  {
    Array<int> vdofs;
    fespace_serial.GetElementVDofs(el, vdofs);
    out << el << " ";
    for (int i = 0; i < vdofs.Size(); ++i)
      out << vdofs[i] << " ";
    out << endl;
  }


  vector<int> my_cells_dofs;
  my_cells_dofs.reserve(fespace.GetNE() * 6);
  for (int el = 0; el < fespace.GetNE(); ++el)
  {
    const int cellID = fespace.GetAttribute(el) - 1;
    Array<int> vdofs;
    fespace.GetElementVDofs(el, vdofs);
    my_cells_dofs.push_back(-cellID);
    my_cells_dofs.push_back(vdofs.Size());
    for (int d = 0; d < vdofs.Size(); ++d) {
      const int globTDof = fespace.GetGlobalTDofNumber(vdofs[d]);
      my_cells_dofs.push_back(globTDof);
    }
  }

  out << "my_cells_dofs:\n";
  for (size_t i = 0; i < my_cells_dofs.size(); ++i) {
    if (my_cells_dofs[i] < 0)
      out << endl;
    out << my_cells_dofs[i] << " ";
  }

  if (myid == 0)
  {
    MPI_Status status;
    for (int rank = 1; rank < nproc; ++rank)
    {
      int ncells_dofs;
      MPI_Recv(&ncells_dofs, 1, MPI_INT, rank, 101, MPI_COMM_WORLD, &status);
      vector<int> cells_dofs(ncells_dofs);
      MPI_Recv(&cells_dofs[0], ncells_dofs, MPI_INT, rank, 102, MPI_COMM_WORLD, &status);
      my_cells_dofs.reserve(my_cells_dofs.size() + ncells_dofs);
//      my_cells_dofs.insert(my_cells_dofs.end(), cells_dofs.begin(), cells_dofs.end());
      for (int i = 0; i < ncells_dofs; ++i)
        my_cells_dofs.push_back(cells_dofs[i]);
    }
  }
  else
  {
    int my_ncells_dofs = my_cells_dofs.size();
    MPI_Send(&my_ncells_dofs, 1, MPI_INT, 0, 101, MPI_COMM_WORLD);
    MPI_Send(&my_cells_dofs[0], my_ncells_dofs, MPI_INT, 0, 102, MPI_COMM_WORLD);
  }

  int nglob_cells_dofs = my_cells_dofs.size();
  MPI_Bcast(&nglob_cells_dofs, 1, MPI_INT, 0, MPI_COMM_WORLD);
  if (myid != 0)
    my_cells_dofs.resize(nglob_cells_dofs);
  MPI_Bcast(&my_cells_dofs[0], nglob_cells_dofs, MPI_INT, 0, MPI_COMM_WORLD);

  const int globNE = param.mesh->GetNE();
  out << "globNE " << globNE << endl;
  vector<vector<int> > map_cell_dofs(globNE);
  for (int el = 0, k = 0; el < globNE; ++el)
  {
    MFEM_VERIFY(k < (int)my_cells_dofs.size(), "k is out of range");
    int cellID = my_cells_dofs[k++];
    MFEM_VERIFY(cellID <= 0, "Incorrect cellID");
    cellID = -cellID;
    const int ndofs = my_cells_dofs[k++];
    if (!(cellID >= 0 && cellID < globNE))
      out << "el " << el << " cellID " << cellID << endl;
    MFEM_VERIFY(cellID >= 0 && cellID < globNE, "cellID is out of range");
    MFEM_VERIFY(map_cell_dofs[cellID].empty(), "This cellID has been already added");
    map_cell_dofs[cellID].resize(ndofs);
    for (int i = 0; i < ndofs; ++i)
      map_cell_dofs[cellID][i] = my_cells_dofs[k++];
  }

  out << "map_cell_dofs:\n";
  for (size_t i = 0; i < map_cell_dofs.size(); ++i) {
    out << i << " ";
    for (size_t j = 0; j < map_cell_dofs[i].size(); ++j)
      out << map_cell_dofs[i][j] << " ";
    out << endl;
  }


  std::vector<int> n_fine_cell_per_coarse_x(param.method.gms_Nx);
  fill_up_n_fine_cells_per_coarse(param.grid.nx, param.method.gms_Nx,
                                  n_fine_cell_per_coarse_x);

  std::vector<int> n_fine_cell_per_coarse_y(param.method.gms_Ny);
  fill_up_n_fine_cells_per_coarse(param.grid.ny, param.method.gms_Ny,
                                  n_fine_cell_per_coarse_y);

  const double hx = param.grid.get_hx();
  const double hy = param.grid.get_hy();

  const int gen_edges = 1;

  std::vector<std::vector<int> > local2global;
  std::vector<DenseMatrix> R;

  if (param.dimension == 2)
  {
    const int n_coarse_cells = param.method.gms_Nx * param.method.gms_Ny;

    // number of coarse cells that all processes will have (at least)
    const int min_n_cells = n_coarse_cells / nproc;
    // number of coarse cells that should be distributed among some processes
    const int extra_cells = n_coarse_cells % nproc;
    // first and last (not including) indices of coarse element for the current
    // 'myid' process
    const int my_start_cell = min_n_cells * myid + (extra_cells < myid ? extra_cells : myid);
    const int my_end_cell   = my_start_cell + min_n_cells + (extra_cells > myid);

    out << "coarse cells: start " << my_start_cell << " end " << my_end_cell << endl;

    local2global.resize(my_end_cell - my_start_cell);
    R.resize(my_end_cell - my_start_cell);

    std::vector<std::vector<int> > local2global_serial(my_end_cell - my_start_cell);

    int offset_x, offset_y = 0;

    for (int iy = 0; iy < param.method.gms_Ny; ++iy)
    {
      const int n_fine_y = n_fine_cell_per_coarse_y[iy];
      const double SY = n_fine_y * hy;

      offset_x = 0;
      for (int ix = 0; ix < param.method.gms_Nx; ++ix)
      {
        const int n_fine_x = n_fine_cell_per_coarse_x[ix];
        const double SX = n_fine_x * hx;

        const int global_coarse_cell = iy*param.method.gms_Nx + ix;
        if (global_coarse_cell < my_start_cell || global_coarse_cell >= my_end_cell)
        {
          offset_x += n_fine_x;
          continue;
        }
        const int my_coarse_cell = global_coarse_cell - my_start_cell;
        out << "\nglobal_coarse_cell " << global_coarse_cell
            << " my_coarse_cell " << my_coarse_cell << endl;

        Mesh *ccell_fine_mesh =
            new Mesh(n_fine_x, n_fine_y, Element::QUADRILATERAL, gen_edges, SX, SY);

        double *local_one_over_rho = new double[n_fine_x * n_fine_y];
        double *local_one_over_K   = new double[n_fine_x * n_fine_y];
        for (int fiy = 0; fiy < n_fine_y; ++fiy)
        {
          for (int fix = 0; fix < n_fine_x; ++fix)
          {
            const int loc_cell = fiy*n_fine_x + fix;
            const int glob_cell = (offset_y + fiy) * param.grid.nx +
                                  (offset_x + fix);

            local_one_over_rho[loc_cell] = one_over_rho[glob_cell];
            local_one_over_K[loc_cell]   = one_over_K[glob_cell];
          }
        }
        const bool own_array = true;
        CWConstCoefficient local_one_over_rho_coef(local_one_over_rho, own_array);
        CWConstCoefficient local_one_over_K_coef(local_one_over_K, own_array);

#ifdef BASIS_DG
        compute_basis_DG(ccell_fine_mesh, param.method.gms_nb, param.method.gms_ni,
                         local_one_over_rho_coef, local_one_over_K_coef,
                         R[my_coarse_cell]);
#else
        compute_basis_CG(ccell_fine_mesh, param.method.gms_nb, param.method.gms_ni,
                         local_one_over_rho_coef, local_one_over_K_coef,
                         R[my_coarse_cell]);
#endif

        // initialize with all -1 to check that all values are defined later
        local2global[my_coarse_cell].resize(R[my_coarse_cell].Height(), -1);
        local2global_serial[my_coarse_cell].resize(R[my_coarse_cell].Height(), -1);
        DG_FECollection DG_fec(param.method.order, param.dimension);
        FiniteElementSpace DG_fespace(ccell_fine_mesh, &DG_fec);
        Array<int> loc_dofs;
        for (int fiy = 0; fiy < n_fine_y; ++fiy)
        {
          for (int fix = 0; fix < n_fine_x; ++fix)
          {
            const int loc_cell = fiy*n_fine_x + fix;
            const int glob_cell = (offset_y + fiy) * param.grid.nx +
                                  (offset_x + fix);
            MFEM_VERIFY(glob_cell >= 0 && glob_cell < (int)map_cell_dofs.size(),
                        "glob_cell is out of range");

            DG_fespace.GetElementVDofs(loc_cell, loc_dofs);
            const vector<int> &glob_dofs = map_cell_dofs[glob_cell];
            MFEM_VERIFY(loc_dofs.Size() == (int)glob_dofs.size(), "Dimensions mismatch");

            Array<int> glob_dofs_serial;
            fespace_serial.GetElementVDofs(glob_cell, glob_dofs_serial);
            MFEM_VERIFY(loc_dofs.Size() == glob_dofs_serial.Size(), "Dimensions mismatch");

            for (int di = 0; di < loc_dofs.Size(); ++di)
              local2global[my_coarse_cell][loc_dofs[di]] = glob_dofs[di];

            for (int di = 0; di < loc_dofs.Size(); ++di)
              local2global_serial[my_coarse_cell][loc_dofs[di]] = glob_dofs_serial[di];
          }
        }

        // check that all values were defined
        for (size_t ii = 0; ii < local2global[my_coarse_cell].size(); ++ii) {
          MFEM_VERIFY(local2global[my_coarse_cell][ii] >= 0, "Some values of local2global "
                      "vector were not defined");
        }
        for (size_t ii = 0; ii < local2global_serial[my_coarse_cell].size(); ++ii) {
          MFEM_VERIFY(local2global_serial[my_coarse_cell][ii] >= 0, "Some values of local2global_serial "
                      "vector were not defined");
        }

        for (size_t ii = 0; ii < local2global[my_coarse_cell].size(); ++ii) {
          out << ii << " " << local2global[my_coarse_cell][ii] << " "
              << local2global_serial[my_coarse_cell][ii] << endl;
          //MFEM_VERIFY(local2global[my_coarse_cell][ii] == local2global_serial[my_coarse_cell][ii],
          //            "local2global matrices mismatch");
        }

        delete ccell_fine_mesh;

        offset_x += n_fine_x;
      }
      offset_y += n_fine_y;
    }
  }
  else // 3D
  {
    /*
    std::vector<int> n_fine_cell_per_coarse_z(param.method.gms_Nz);
    fill_up_n_fine_cells_per_coarse(param.grid.nz, param.method.gms_Nz,
                                    n_fine_cell_per_coarse_z);

    const double hz = param.grid.get_hz();

    const int n_coarse_cells = param.method.gms_Nx * param.method.gms_Ny * param.method.gms_Nz;
    local2global.resize(n_coarse_cells);
    R.resize(n_coarse_cells);

    int offset_x = 0, offset_y = 0, offset_z = 0;

    for (int iz = 0; iz < param.method.gms_Nz; ++iz)
    {
      const int n_fine_z = n_fine_cell_per_coarse_z[iz];
      const double SZ = n_fine_z * hz;
      for (int iy = 0; iy < param.method.gms_Ny; ++iy)
      {
        const int n_fine_y = n_fine_cell_per_coarse_y[iy];
        const double SY = n_fine_y * hy;
        for (int ix = 0; ix < param.method.gms_Nx; ++ix)
        {
          const int n_fine_x = n_fine_cell_per_coarse_x[ix];
          const double SX = n_fine_x * hx;
          Mesh *ccell_fine_mesh =
              new Mesh(n_fine_cell_per_coarse_x[ix],
                       n_fine_cell_per_coarse_y[iy],
                       n_fine_cell_per_coarse_z[iz],
                       Element::HEXAHEDRON, gen_edges, SX, SY, SZ);

          double *local_one_over_rho = new double[n_fine_x * n_fine_y * n_fine_z];
          double *local_one_over_K   = new double[n_fine_x * n_fine_y * n_fine_z];
          for (int fiz = 0; fiz < n_fine_z; ++fiz)
          {
            for (int fiy = 0; fiy < n_fine_y; ++fiy)
            {
              for (int fix = 0; fix < n_fine_x; ++fix)
              {
                const int loc_cell = fiz*n_fine_x*n_fine_y + fiy*n_fine_x + fix;
                const int glob_cell = (offset_z + fiz) * param.grid.nx * param.grid.ny +
                                      (offset_y + fiy) * param.grid.nx +
                                      (offset_x + fix);

                local_one_over_rho[loc_cell] = one_over_rho[glob_cell];
                local_one_over_K[loc_cell]   = one_over_K[glob_cell];
              }
            }
          }
          const bool own_array = true;
          CWConstCoefficient local_one_over_rho_coef(local_one_over_rho, own_array);
          CWConstCoefficient local_one_over_K_coef(local_one_over_K, own_array);

#ifdef BASIS_DG
          compute_basis_DG(ccell_fine_mesh, param.method.gms_nb, param.method.gms_ni,
                           local_one_over_rho_coef, local_one_over_K_coef,
                           R[iz*param.method.gms_Nx*param.method.gms_Ny +
                             iy*param.method.gms_Nx + ix]);
#else
          compute_basis_CG(ccell_fine_mesh, param.method.gms_nb, param.method.gms_ni,
                           local_one_over_rho_coef, local_one_over_K_coef,
                           R[iz*param.method.gms_Nx*param.method.gms_Ny +
                             iy*param.method.gms_Nx + ix]);
#endif
          delete ccell_fine_mesh;

          offset_x += n_fine_x;
        }
        offset_y += n_fine_y;
      }
      offset_z += n_fine_z;
    }
    */
  }

  // global sparse R matrix
  int my_nrows = 0;
  int my_ncols = 0;
  int my_nnonzero = 0;
  for (size_t i = 0; i < R.size(); ++i)
  {
    const int h = R[i].Height();
    const int w = R[i].Width();
    my_nrows += w; // transpose
    my_ncols += h; // transpose
    my_nnonzero += h * w;
  }

  int glob_nrows = 0;
  int glob_ncols = 0;

  MPI_Allreduce(&my_nrows, &glob_nrows, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&my_ncols, &glob_ncols, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

  out << "\nmy_nrows " << my_nrows << " my_ncols " << my_ncols << endl;
  out << "glob_nrows " << glob_nrows << " glob_ncols " << glob_ncols << endl;

  int *Rrows = new int[nproc + 1];
  if (myid == 0)
  {
    Rrows[0] = 0;
    Rrows[1] = my_nrows;
    MPI_Status status;
    for (int rank = 1; rank < nproc; ++rank)
    {
      int nrows;
      MPI_Recv(&nrows, 1, MPI_INT, rank, 103, MPI_COMM_WORLD, &status);
      Rrows[rank + 1] = Rrows[rank] + nrows;
    }
  }
  else
  {
    MPI_Send(&my_nrows, 1, MPI_INT, 0, 103, MPI_COMM_WORLD);
  }
  MPI_Bcast(Rrows, nproc + 1, MPI_INT, 0, MPI_COMM_WORLD);

  out << "\nRrows: ";
  for (int i = 0; i < nproc + 1; ++i)
    out << Rrows[i] << " ";
  out << endl;

  const int my_start_row = Rrows[myid];
  const int my_end_row   = Rrows[myid + 1];
  MFEM_VERIFY(my_nrows == my_end_row - my_start_row, "Number of rows mismatch");

  const int *S_row_starts = S_fine->RowPart();
  const int *S_col_starts = S_fine->ColPart();
  const int *M_row_starts = M_fine->RowPart();
  const int *M_col_starts = M_fine->ColPart();
  out << "S_row_starts: " << S_row_starts[0] << " " << S_row_starts[1] << endl;
  out << "S_col_starts: " << S_col_starts[0] << " " << S_col_starts[1] << endl;
  out << "M_row_starts: " << M_row_starts[0] << " " << M_row_starts[1] << endl;
  out << "M_col_starts: " << M_col_starts[0] << " " << M_col_starts[1] << endl;

  int *Ri = new int[my_nrows + 1];
  int *Rj = new int[my_nnonzero];
  double *Rdata = new double[my_nnonzero];

  Ri[0] = 0;
  int k = 0;
  int p = 0;
  for (size_t r = 0; r < R.size(); ++r)
  {
    const int h = R[r].Height();
    const int w = R[r].Width();
    for (int i = 0; i < w; ++i)
    {
      Ri[k+1] = Ri[k] + h;
      ++k;

      for (int j = 0; j < h; ++j)
      {
        Rj[p] = local2global[r][j];
        Rdata[p] = R[r](j, i);
        ++p;
      }
    }
  }

  int myRrows[] = { my_start_row, my_end_row };

  /** Creates a general parallel matrix from a local CSR matrix on each
      processor described by the I, J and data arrays. The local matrix should
      be of size (local) nrows by (global) glob_ncols. The new parallel matrix
      contains copies of all input arrays (so they can be deleted). */
  HypreParMatrix R_global(MPI_COMM_WORLD, my_nrows, glob_nrows, glob_ncols,
                          Ri, Rj, Rdata, myRrows, S_fine->RowPart());

  HypreParMatrix *R_global_T = R_global.Transpose();

  delete[] Rrows;
  delete[] Rdata;
  delete[] Rj;
  delete[] Ri;

  HypreParMatrix *M_coarse = RAP(M_fine, R_global_T);
  HypreParMatrix *S_coarse = RAP(S_fine, R_global_T);
  HypreParVector b_coarse(*M_coarse);
  R_global.Mult(*B_fine, b_coarse);

  if (param.output.print_matrices)
  {
    {
      chrono.Clear();
      cout << "Output R local matrices..." << flush;
      for (size_t r = 0; r < R.size(); ++r) {
        const string fname = string(param.output.directory) + "/r" + d2s(r) +
                             "_local_par" + param.output.extra_string + ".dat";
        ofstream mout(fname.c_str());
        MFEM_VERIFY(mout, "Cannot open file " + fname);
        R[r].PrintMatlab(mout);
      }
      cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
    }
    {
      chrono.Clear();
      cout << "Output R_global matrix..." << flush;
      const string fname = string(param.output.directory) + "/r_global_par" +
                           param.output.extra_string + ".dat";
      print_par_matrix_matlab(R_global, fname);
      cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
    }
    {
      chrono.Clear();
      cout << "Output R_global_T matrix..." << flush;
      const string fname = string(param.output.directory) + "/r_global_t_par" +
                           param.output.extra_string + ".dat";
      print_par_matrix_matlab(*R_global_T, fname);
      cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
    }
    {
      chrono.Clear();
      cout << "Output M_fine matrix..." << flush;
      const string fname = string(param.output.directory) + "/m_fine_par" +
                           param.output.extra_string + ".dat";
      print_par_matrix_matlab(*M_fine, fname);
      cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
    }
    {
      chrono.Clear();
      cout << "Output S_fine matrix..." << flush;
      const string fname = string(param.output.directory) + "/s_fine_par" +
                           param.output.extra_string + ".dat";
      print_par_matrix_matlab(*S_fine, fname);
      cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
    }
    {
      chrono.Clear();
      cout << "Output M_coarse matrix..." << flush;
      const string fname = string(param.output.directory) + "/m_coarse_par" +
                           param.output.extra_string + ".dat";
      print_par_matrix_matlab(*M_coarse, fname);
      cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
    }
    {
      chrono.Clear();
      cout << "Output S_coarse matrix..." << flush;
      const string fname = string(param.output.directory) + "/s_coarse_par" +
                           param.output.extra_string + ".dat";
      print_par_matrix_matlab(*S_coarse, fname);
      cout << "done. Time = " << chrono.RealTime() << " sec" << endl;
    }
  }

  const string method_name = "parGMsFEM_";

  HypreParVector U_0(*M_coarse); U_0 = 0.0;
  HypreParVector U_1(*M_coarse); U_1 = 0.0;
  HypreParVector U_2(*M_coarse); U_2 = 0.0;
  ParGridFunction u_0(&fespace);

  const int n_time_steps = param.T / param.dt + 0.5; // nearest integer
  const int tenth = 0.1 * n_time_steps;

  if (myid == 0)
    cout << "N time steps = " << n_time_steps
         << "\nTime loop..." << endl;

  // the values of the time-dependent part of the source
  vector<double> time_values(n_time_steps);
  for (int time_step = 1; time_step <= n_time_steps; ++time_step)
  {
    const double cur_time = time_step * param.dt;
    time_values[time_step-1] = RickerWavelet(param.source,
                                             cur_time - param.dt);
  }

  const string name = method_name + param.output.extra_string;
  const string pref_path = string(param.output.directory) + "/" + SNAPSHOTS_DIR;
  VisItDataCollection visit_dc(name.c_str(), param.par_mesh);
  visit_dc.SetPrefixPath(pref_path.c_str());
  visit_dc.RegisterField("coarse_pressure", &u_0);
  {
    visit_dc.SetCycle(0);
    visit_dc.SetTime(0.0);
    HypreParVector u_tmp(&fespace);
    R_global_T->Mult(U_0, u_tmp);
    u_0 = u_tmp;
    visit_dc.Save();
  }

  StopWatch time_loop_timer;
  time_loop_timer.Start();
  double time_of_snapshots = 0.;
  double time_of_seismograms = 0.;
  for (int t_step = 1; t_step <= n_time_steps; ++t_step)
  {
    {
      par_time_step(*M_coarse, *S_coarse, b_coarse, time_values[t_step-1],
                    param.dt, U_0, U_1, U_2); //, out);
    }

    // Compute and print the L^2 norm of the error
    double glob_norm = GlobalLpNorm(2, U_0.Norml2(), MPI_COMM_WORLD);
    if (t_step % tenth == 0) {
      out << "step " << t_step << " / " << n_time_steps
           << " ||U||_{L^2} = " << glob_norm << endl;
    }

    {
      StopWatch timer;
      timer.Start();
      HypreParVector u_tmp(&fespace);
      R_global_T->Mult(U_0, u_tmp);
      //{ double norm = GlobalLpNorm(2, u_tmp.Norml2(), MPI_COMM_WORLD); out << "||utmp_H|| = " << norm << endl; }
      if (t_step % param.step_snap == 0) {
        visit_dc.SetCycle(t_step);
        visit_dc.SetTime(t_step*param.dt);
        u_0 = u_tmp;
        visit_dc.Save();
      }
      timer.Stop();
      time_of_snapshots += timer.UserTime();
    }

    if (t_step % param.step_snap == 0)
    {
      ostringstream mesh_name, sol_name;
      mesh_name << param.output.directory << "/" << param.output.extra_string << "_mesh." << setfill('0') << setw(6) << myid;
      sol_name << param.output.directory << "/" << param.output.extra_string << "_sol_t" << t_step << "." << setfill('0') << setw(6) << myid;

      ofstream mesh_ofs(mesh_name.str().c_str());
      mesh_ofs.precision(8);
      param.par_mesh->Print(mesh_ofs);

      ofstream sol_ofs(sol_name.str().c_str());
      sol_ofs.precision(8);
      HypreParVector u_tmp(&fespace);
      R_global_T->Mult(U_0, u_tmp);
      ParGridFunction x(&fespace, &u_tmp);
      x.Save(sol_ofs);
    }

//    if (t_step % param.step_seis == 0) {
//      StopWatch timer;
//      timer.Start();
//      R_global_T.Mult(U_0, u_0);
//      output_seismograms(param, *param.mesh, u_0, seisU);
//      timer.Stop();
//      time_of_seismograms += timer.UserTime();
//    }
  }

  time_loop_timer.Stop();

  delete S_coarse;
  delete M_coarse;
  delete R_global_T;

  delete B_fine;
  delete M_fine;
  delete S_fine;
}
#endif // MFEM_USE_MPI

