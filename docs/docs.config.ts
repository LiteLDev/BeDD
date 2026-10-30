// Be.D.D. documentation — levimc-kit site config, served at https://docs.levimc.org/BeDD/.
// English at the root, Simplified Chinese in zh/, Traditional Chinese converted from it.
import { defineDocsConfig, type NavEntry } from './kit/docs/node/config.ts';

const repo = 'https://github.com/LiteLDev/BeDD';

interface Labels {
  guide: string;
  reference: string;
  gallery: string;
  start: string;
  introduction: string;
  installation: string;
  newPc: string;
  scripts: string;
  usage: string;
  severalGames: string;
  harness: string;
  configuration: string;
  troubleshooting: string;
  protocol: string;
  tap: string;
  howItWorks: string;
  building: string;
}

const LABELS: Record<string, Labels> = {
  root: {
    guide: 'Guide',
    reference: 'Reference',
    gallery: 'Gallery',
    start: 'Getting started',
    introduction: 'Introduction',
    installation: 'Installation',
    newPc: 'Setting up a new PC',
    scripts: 'Writing tests',
    usage: 'Driving the game',
    severalGames: 'Several games at once',
    harness: 'Plugging into a harness',
    configuration: 'Configuration',
    troubleshooting: 'Troubleshooting',
    protocol: 'Control pipe protocol',
    tap: 'Tap protocol',
    howItWorks: 'How it works',
    building: 'Building and contributing',
  },
  zh: {
    guide: '指南',
    reference: '参考',
    gallery: '展示',
    start: '入门',
    introduction: '简介',
    installation: '安装',
    newPc: '在新电脑上准备',
    scripts: '编写测试',
    usage: '驱动游戏',
    severalGames: '多开',
    harness: '接入测试框架',
    configuration: '配置',
    troubleshooting: '常见问题',
    protocol: '控制管道协议',
    tap: 'Tap 协议',
    howItWorks: '工作原理',
    building: '构建与贡献',
  },
};

const nav = (p: string, t: Labels): NavEntry[] => [
  { text: t.guide, link: `${p}guide/introduction` },
  { text: t.reference, link: `${p}reference/protocol` },
  { text: t.gallery, link: `${p}gallery` },
];

const sidebar = (p: string, t: Labels): Record<string, NavEntry[]> => {
  const all: NavEntry[] = [
    {
      text: t.start,
      items: [
        { text: t.introduction, link: `${p}guide/introduction` },
        { text: t.installation, link: `${p}guide/installation` },
        { text: t.newPc, link: `${p}guide/new-pc` },
      ],
    },
    {
      text: t.scripts,
      items: [
        { text: t.usage, link: `${p}guide/usage` },
        { text: t.severalGames, link: `${p}guide/several-games` },
        { text: t.harness, link: `${p}guide/harness` },
        { text: t.configuration, link: `${p}guide/configuration` },
        { text: t.troubleshooting, link: `${p}guide/troubleshooting` },
      ],
    },
    {
      text: t.reference,
      items: [
        { text: t.protocol, link: `${p}reference/protocol` },
        { text: t.tap, link: `${p}reference/tap` },
        { text: t.howItWorks, link: `${p}reference/how-it-works` },
        { text: t.building, link: `${p}reference/building` },
      ],
    },
  ];
  return { [`${p}guide/`]: all, [`${p}reference/`]: all };
};

export default defineDocsConfig({
  base: '/BeDD/',
  title: 'Be.D.D.',
  description: 'Run end-to-end tests of LeviLamina client mods in the background: scripts drive Minecraft Bedrock while your mouse, keyboard and windows stay yours.',
  project: 'BeDD',
  logo: { src: '/logo.png', alt: 'Be.D.D.' },
  favicon: '/logo.png',
  contentDir: '.',
  repo,
  editLink: `${repo}/edit/main/docs/:path`,
  hostname: 'https://docs.levimc.org',
  sitemap: { hostname: 'https://docs.levimc.org' },
  i18n: {
    mode: 'folder',
    defaultLocale: 'root',
    locales: [
      { code: 'root', label: 'English', lang: 'en', prefix: '' },
      { code: 'zh', label: '简体中文', lang: 'zh-CN', prefix: 'zh/' },
      { code: 'zh-Hant', label: '繁體中文', lang: 'zh-Hant', prefix: 'zh-Hant/', convertFrom: 'zh' },
    ],
  },
  urlStyle: 'html',
  cleanUrls: true,
  slug: 'vitepress',
  markdown: { compat: 'commonmark', containers: { vitepress: true } },
  outline: [2, 3],
  exclude: ['scripts/**', 'README.md'],
  nav: { root: nav('/', LABELS.root!), zh: nav('/zh/', LABELS.zh!) },
  sidebar: { root: sidebar('/', LABELS.root!), zh: sidebar('/zh/', LABELS.zh!) },
  social: [{ icon: 'github', link: repo, label: 'GitHub' }],
});
