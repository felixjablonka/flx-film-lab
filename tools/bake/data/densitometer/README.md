# Densitometer responsivities

ISO 5-3 Status A (transparencies) and Status M (negatives) spectral
responsivities, linear, one CSV per channel as `wavelength_nm, responsivity`.
They are agx-emulsion's digitisation (spektrafilm's history, removed from its
public tree in a6dbcfd), after Giorgianni & Madden, *Digital Color Management*
(Wiley, 2009). `build_profiles.py` reads them to turn a datasheet's integral
densities into dye amounts with the donor stock's dye spectra.
