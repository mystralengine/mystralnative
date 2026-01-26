import { useState } from 'react';
import { Link } from 'react-router-dom';

function detectOS(): 'unix' | 'windows' {
  if (typeof navigator === 'undefined') return 'unix';
  const ua = navigator.userAgent.toLowerCase();
  if (ua.includes('win')) return 'windows';
  return 'unix';
}

export default function HomePage() {
  const [tab, setTab] = useState<'unix' | 'windows'>(detectOS);
  const [copied, setCopied] = useState(false);

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

      <div style={{ marginTop: 'var(--navbar-height)' }}>
        <section className="hero">
          <img
            src="/mystralnative/mystralnative.png"
            alt="Mystral Native.js"
            style={{ maxWidth: '500px', width: '100%', marginBottom: '24px', borderRadius: '12px' }}
          />
          <p>
            Run WebGPU games natively with JavaScript. Write once, run everywhere —
            macOS, Windows, Linux, iOS, and Android.
          </p>

          <div className="install-tabs">
            <div className="install-tab-buttons">
              <button
                className={`install-tab-btn ${tab === 'unix' ? 'active' : ''}`}
                onClick={() => setTab('unix')}
              >
                macOS / Linux
              </button>
              <button
                className={`install-tab-btn ${tab === 'windows' ? 'active' : ''}`}
                onClick={() => setTab('windows')}
              >
                Windows
              </button>
              <button
                className="install-copy-btn"
                onClick={async () => {
                  const text = tab === 'unix'
                    ? 'curl -fsSL https://mystralengine.github.io/mystralnative/install.sh | bash'
                    : 'irm https://mystralengine.github.io/mystralnative/install.ps1 | iex';
                  await navigator.clipboard.writeText(text);
                  setCopied(true);
                  setTimeout(() => setCopied(false), 1000);
                }}
              >
                {copied ? 'Copied!' : 'Copy'}
              </button>
            </div>
            <div className="install-command">
              <code>
                {tab === 'unix'
                  ? 'curl -fsSL https://mystralengine.github.io/mystralnative/install.sh | bash'
                  : 'irm https://mystralengine.github.io/mystralnative/install.ps1 | iex'}
              </code>
            </div>
          </div>

          <div className="hero-buttons">
            <Link to="/docs/getting-started" className="btn btn-primary">
              Get Started
            </Link>
            <a
              href="https://github.com/mystralengine/mystralnative/releases"
              className="btn btn-secondary"
              target="_blank"
              rel="noopener"
            >
              Download
            </a>
          </div>
        </section>

        <section className="features">
          <div className="feature">
            <h3>Native Performance</h3>
            <p>
              Run WebGPU games at native speed with Dawn or wgpu-native backends.
              No browser overhead.
            </p>
          </div>

          <div className="feature">
            <h3>Multiple JS Engines</h3>
            <p>
              Choose from V8, QuickJS, or JavaScriptCore. Pick the right engine
              for your platform and use case.
            </p>
          </div>

          <div className="feature">
            <h3>Cross-Platform</h3>
            <p>
              Build for macOS (arm64/x64), Windows, Linux, iOS, and Android
              from a single JavaScript codebase.
            </p>
          </div>

          <div className="feature">
            <h3>Web Audio Support</h3>
            <p>
              Full Web Audio API implementation powered by SDL3 for
              cross-platform audio playback.
            </p>
          </div>

          <div className="feature">
            <h3>Easy Distribution</h3>
            <p>
              Ship your game as a single binary. No dependencies, no installers,
              just download and run.
            </p>
          </div>

          <div className="feature">
            <h3>Embeddable</h3>
            <p>
              Embed the runtime in your own applications. Use as a library
              for iOS and Android native apps.
            </p>
          </div>
        </section>
      </div>
    </>
  );
}
