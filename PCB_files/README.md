<p align="center" width="100%">
  <img alt="Logo" width="33%" src="Logos/pd240w_logo.png">
</p>

<h1 align="center">PD240W</h1>

<p align="center" width="100%">
    <img src="Images/PCB_naked.jpg">
</p>

***

<p align="center">
  <img alt="3D Top Angled" src="Images/PD240W_kicad-angled_top.png" width="45%">
&nbsp; &nbsp; &nbsp; &nbsp;
  <img alt="3D Bottom Angled" src="Images/PD240W_kicad-angled_bottom.png" width="45%">
</p>

***

## SPECIFICATIONS

| Parameter | Value | 
| --- | --- |
| Dimensions | 66.0 × 66.0 mm |
| Input Voltage | 5 - 53 V |
| Output Voltage | 3.3 - 48 V |
| Max. Output Current Voltage | 5 A |
| Max Power Output | 240 W |

***

## DIRECTORY STRUCTURE

    .
    ├─ Computations       # Misc calculations
    ├─ HTML               # HTML files for generated webpage
    ├─ Images             # Pictures and renders
    │
    ├─ kibot_resources    # External resources for KiBot
    │  ├─ colors          # Color theme for KiCad
    │  ├─ fonts           # Fonts used in the project
    │  ├─ scripts         # External scripts used with KiBot
    │  └─ templates       # Templates for KiBot generated reports
    │
    ├─ kibot_yaml         # KiBot YAML config files
    ├─ KiRI               # KiRI (PCB diff viewer) files
    │
    ├─ lib                # KiCad footprint and symbol libraries
    │  ├─ 3d_models       # Component 3D models
    │  ├─ lib_fp          # Footprint libraries
    │  └─ lib_sym         # Symbol libraries
    │
    ├─ Logos              # Logos
    │
    ├─ Manufacturing      # Assembly and fabrication documents
    │  ├─ Assembly        # Assembly documents (BoM, pos, notes)
    │  │
    │  └─ Fabrication     # Fabrication documents (ZIP, notes)
    │     ├─ Drill Tables # CSV drill tables
    │     └─ Gerbers      # Gerbers
    │
    ├─ Report             # Reports for ERC/DRC
    ├─ Schematic          # PDF of schematic
    ├─ Templates          # Title block templates
    ├─ Testing
    │  └─ Testpoints      # Testpoints tables      
    │
    └─ Variants           # Outputs for assembly variants
