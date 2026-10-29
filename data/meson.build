pfiles = [
  'controller_mask.png',
  'controller_mask_s.png',
  'xmu_mask.png',
  'logo_sdf.png',
  'xemu_64x64.png',
  'abxy.ttf',
  'Roboto-Medium.ttf',
  'RobotoCondensed-Regular.ttf',
  'RussoOne-Regular.ttf',  # SIL OFL, see RussoOne-OFL.txt; the Original Xbox dashboard styles
  'shader_seeds.bin',  # empty dictionary: Xbox seeds are recorded per game on this machine
  'font_awesome_6_1_1_solid.min.otf',  # Update glyphs with scripts/gen-min-fa-font.sh
]

libpfile_targets = []

foreach e : pfiles
  t = custom_target('@0@.[ch]'.format(e),
                    output: ['@0@.c'.format(e), '@0@.h'.format(e)],
                    input: files('@0@'.format(e)),
                    command: [packfile, '@INPUT@', '@OUTPUT0@', '@OUTPUT1@'])
  libpfile_targets += t[0]
  genh += t[1]
endforeach

libpfile = static_library('pdata', libpfile_targets,
                          name_suffix: 'fa')
common_ss.add(declare_dependency(link_whole: libpfile))
