#ifndef PARAMETERS_HPP
#define PARAMETERS_HPP

#include "config.hpp"
#include "mfem.hpp"
#include "source.hpp"

#include <string>
#include <vector>

static const char DEFAULT_FILE_NAME[] = "no-file";

class ReceiversSet;
class SnapshotsSet;



/**
 * Parameters describing the domain and the grid.
 */
class GridParameters
{
public:
  GridParameters();
  ~GridParameters() { }

  double sx, sy, sz; ///< size of the computational domain
  int nx, ny, nz; ///< number of cells in x-, y- and z-directions

  const char* meshfile; ///< name of file with mesh

  double get_hx() const { return sx / nx; }
  double get_hy() const { return sy / ny; }
  double get_hz() const { return sz / nz; }

  void AddOptions(mfem::OptionsParser& args);
  void check_parameters(int dim) const;

private:
  GridParameters(const GridParameters&);
  GridParameters& operator=(const GridParameters&);
};



/**
 * Parameters describing the source.
 */
class SourceParameters
{
public:
  SourceParameters();
  ~SourceParameters() { }

  mfem::Vertex location;
  double frequency;
  double scale;
  const char *spatial_function; ///< "delta", "gauss"
  double gauss_support; ///< size of the support for the "gauss" spatial function
  bool plane_wave; ///< plane wave as a source at the depth of y-coordinate of
                   ///< the source location

  void AddOptions(mfem::OptionsParser& args);
  void check_parameters() const;

private:
  SourceParameters(const SourceParameters&);
  SourceParameters& operator=(const SourceParameters&);
};



/**
 * Parameters describing the media properties.
 */
class MediaPropertiesParameters
{
public:
  MediaPropertiesParameters();
  ~MediaPropertiesParameters();

  double rho, vp; ///< homogeneous media properties

  const char *rhofile; ///< file names for heterogeneous media properties
  const char *vpfile;

  double *rho_array, *vp_array; ///< arrays of values describing media
                                ///< properties

  double min_rho, max_rho, min_vp, max_vp;

  void AddOptions(mfem::OptionsParser& args);
  void check_parameters() const;
  void init(int n_elements);

private:
  MediaPropertiesParameters(const MediaPropertiesParameters&);
  MediaPropertiesParameters& operator=(const MediaPropertiesParameters&);
};



/**
 * Parameters describing the boundary conditions.
 */
class BoundaryConditionsParameters
{
public:
  BoundaryConditionsParameters();
  ~BoundaryConditionsParameters() { }

  const char* left;   ///< left surface   (X=0) : absorbing (abs) or free
  const char* right;  ///< right surface  (X=sx): absorbing (abs) or free
  const char* bottom; ///< bottom surface (Y=0) : absorbing (abs) or free
  const char* top;    ///< top surface    (Y=sy): absorbing (abs) or free
  const char* front;  ///< front surface  (Z=0) : absorbing (abs) or free
  const char* back;   ///< back surface   (Z=sz): absorbing (abs) or free
  double damp_layer; ///< thickness of a damping layer
  double damp_power; ///< power in damping coefficient functions

  void AddOptions(mfem::OptionsParser& args);
  void check_parameters() const;

private:
  BoundaryConditionsParameters(const BoundaryConditionsParameters&);
  BoundaryConditionsParameters& operator=(const BoundaryConditionsParameters&);
};

/**
 * Parameters describing the method and some specific parameters of the method
 * (if any).
 */
class MethodParameters
{
public:
  MethodParameters();
  ~MethodParameters() { }

  int order; ///< finite element order
  const char *name; ///< FEM, SEM, DG, GMsFEM

  /**
   * Parameters of the DG method.
   * sigma = -1, kappa >= kappa0: symm. interior penalty (IP or SIPG) method,
   * sigma = +1, kappa > 0: non-symmetric interior penalty (NIPG) method,
   * sigma = +1, kappa = 0: the method of Baumann and Oden
   */
  double dg_sigma, dg_kappa;

  /**
   * Parameters of the GMsFEM method
   */
  int gms_Nx, gms_Ny, gms_Nz; // number of coarse cells
  int gms_nb, gms_ni; // number of basis functions


  void AddOptions(mfem::OptionsParser& args);
  void check_parameters() const;

private:
  MethodParameters(const MethodParameters&);
  MethodParameters& operator=(const MethodParameters&);
};

/**
 * Handle output results and intermediate steps of computations
 */
class OutputParameters
{
public:
  OutputParameters();
  ~OutputParameters() { }

  const char *directory; ///< directory for saving results of computations
  const char *extra_string; ///< added to output files for distinguishing the
                            ///< results

  bool print_matrices; ///< output (print to a file) matrices (yes, no)
  bool view_snapshot_space; ///< view some intermediate results via GLVis
  bool view_boundary_basis;
  bool view_interior_basis;
  bool view_dg_basis;

  void AddOptions(mfem::OptionsParser& args);
  void check_parameters() const;

private:
  OutputParameters(const OutputParameters&);
  OutputParameters& operator =(const OutputParameters&);
};



/**
 * Parameters of the problem to be solved.
 */
class Parameters
{
public:
  Parameters();
  ~Parameters();

  int dimension; ///< 2D or 3D simulation

  GridParameters grid;
  SourceParameters source;
  MediaPropertiesParameters media;
  BoundaryConditionsParameters bc;
  MethodParameters method;
  OutputParameters output;

  mfem::Mesh *mesh;
  mfem::ParMesh *par_mesh;

  double T; ///< simulation time
  double dt; ///< time step

  int step_snap; ///< time step for outputting snapshots (every *th time step)
  int step_seis; ///< time step for outputting seismograms (every *th time step)
  const char *receivers_file; ///< file describing the sets of receivers
  std::vector<ReceiversSet*> sets_of_receivers;

  void init(int argc, char **argv);
  void check_parameters() const;

private:
  Parameters(const Parameters&); // no copies
  Parameters& operator=(const Parameters&); // no copies
};

#endif // PARAMETERS_HPP
