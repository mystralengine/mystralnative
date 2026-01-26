import { useEffect, useState } from 'react';
import { Link, useLocation } from 'react-router-dom';

// Import all markdown files at build time
const mdxModules = import.meta.glob('../../docs/**/*.{md,mdx}');

// Sidebar configuration
const sidebarItems = [
  {
    title: 'Getting Started',
    items: [
      { label: 'Introduction', path: 'getting-started' },
      { label: 'Installation', path: 'installation' },
      { label: 'Quick Start', path: 'quick-start' },
      { label: 'Uninstalling', path: 'uninstalling' },
    ],
  },
  {
    title: 'Guides',
    items: [
      { label: 'Running Games', path: 'guides/running-games' },
      { label: 'Building from Source', path: 'guides/building' },
      { label: 'Configuration', path: 'guides/configuration' },
    ],
  },
  {
    title: 'API Reference',
    items: [
      { label: 'CLI Commands', path: 'api/cli' },
      { label: 'JavaScript APIs', path: 'api/javascript' },
      { label: 'Embedding', path: 'api/embedding' },
    ],
  },
  {
    title: 'Platform Guides',
    items: [
      { label: 'macOS', path: 'platforms/macos' },
      { label: 'Windows', path: 'platforms/windows' },
      { label: 'Linux', path: 'platforms/linux' },
      { label: 'iOS', path: 'platforms/ios' },
      { label: 'Android', path: 'platforms/android' },
    ],
  },
];

export default function DocPage() {
  const location = useLocation();
  const [Content, setContent] = useState<React.ComponentType | null>(null);
  const [error, setError] = useState<string | null>(null);

  // Extract slug from path (e.g., /docs/getting-started -> getting-started)
  const slug = location.pathname.replace('/docs/', '') || 'getting-started';

  useEffect(() => {
    async function loadContent() {
      setError(null);
      setContent(null);

      // Try different path variations
      const possiblePaths = [
        `../../docs/${slug}.mdx`,
        `../../docs/${slug}.md`,
        `../../docs/${slug}/index.mdx`,
        `../../docs/${slug}/index.md`,
      ];

      for (const path of possiblePaths) {
        if (mdxModules[path]) {
          try {
            const module = (await mdxModules[path]()) as { default: React.ComponentType };
            setContent(() => module.default);
            return;
          } catch (err) {
            console.error('Failed to load:', path, err);
          }
        }
      }

      setError(`Document not found: ${slug}`);
    }

    loadContent();
  }, [slug]);

  return (
    <>
      <nav className="navbar">
        <Link to="/" className="navbar-brand">
          Mystral Native.js
        </Link>
        <div className="navbar-links">
          <Link to="/docs/getting-started">Docs</Link>
          <a href="https://github.com/mystralengine/mystralnative" target="_blank" rel="noopener">
            GitHub
          </a>
        </div>
      </nav>

      <div className="layout">
        <aside className="sidebar">
          {sidebarItems.map((section) => (
            <div key={section.title} className="sidebar-section">
              <div className="sidebar-title">{section.title}</div>
              {section.items.map((item) => (
                <Link
                  key={item.path}
                  to={`/docs/${item.path}`}
                  className={`sidebar-link ${slug === item.path ? 'active' : ''}`}
                >
                  {item.label}
                </Link>
              ))}
            </div>
          ))}
        </aside>

        <main className="content">
          {error ? (
            <div>
              <h1>Page Not Found</h1>
              <p>{error}</p>
              <p>
                <Link to="/docs/getting-started">Go to Getting Started</Link>
              </p>
            </div>
          ) : Content ? (
            <Content />
          ) : (
            <div>Loading...</div>
          )}
        </main>
      </div>
    </>
  );
}
